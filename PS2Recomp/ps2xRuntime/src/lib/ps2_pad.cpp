#include "runtime/ps2_pad.h"
#include "ps2_host_backend.h"
#include "runtime/ps2_mod_loader.h"
#include "runtime/ps2_netplay.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <cstdlib>
#include <string>
#include <vector>

#if defined(PS2X_SDL_PAD)
#define SDL_MAIN_HANDLED
#include <SDL.h>
#endif

extern std::atomic<uint64_t> g_ps2xVsyncTickCount; // EeScheduler.cpp: emulated VBlanks since boot

namespace
{
    constexpr uint8_t kPadAnalogMarker = 0x73;
    constexpr uint8_t kPadStickCenter = 0x80;

    constexpr uint16_t PAD_LEFT = 0x0080u;
    constexpr uint16_t PAD_DOWN = 0x0040u;
    constexpr uint16_t PAD_RIGHT = 0x0020u;
    constexpr uint16_t PAD_UP = 0x0010u;
    constexpr uint16_t PAD_START = 0x0008u;
    constexpr uint16_t PAD_R3 = 0x0004u;
    constexpr uint16_t PAD_L3 = 0x0002u;
    constexpr uint16_t PAD_SELECT = 0x0001u;
    constexpr uint16_t PAD_SQUARE = 0x8000u;
    constexpr uint16_t PAD_CROSS = 0x4000u;
    constexpr uint16_t PAD_CIRCLE = 0x2000u;
    constexpr uint16_t PAD_TRIANGLE = 0x1000u;
    constexpr uint16_t PAD_R1 = 0x0800u;
    constexpr uint16_t PAD_L1 = 0x0400u;
    constexpr uint16_t PAD_R2 = 0x0200u;
    constexpr uint16_t PAD_L2 = 0x0100u;

    // PS2X_AUTOPAD="start@25-26,cross@40-40.5,rx+@50-51": hold buttons (or push a stick fully:
    // lx-/lx+/ly-/ly+/rx-/rx+/ry-/ry+) during these seconds after the first pad read. For headless
    // test runs (no keyboard in Xvfb/CI). PS2X_AUTOPAD_CLOCK=vblank: the times are emulated seconds (VBlanks / 60)
    // instead of host seconds, so a route lands on the same game moment on a slow PC (menus run below full speed).
    struct AutoPress
    {
        uint16_t bit;
        double from;
        double to;
        int axis = -1; // stick byte (0 rx, 1 ry, 2 lx, 3 ly) instead of a button
        uint8_t value = 0;
    };
    const std::vector<AutoPress> &autoPresses()
    {
        static const std::vector<AutoPress> presses = []
        {
            std::vector<AutoPress> out;
            const char *env = std::getenv("PS2X_AUTOPAD");
            if (!env)
                return out;
            const struct { const char *name; uint16_t bit; } names[] = {
                {"start", PAD_START}, {"select", PAD_SELECT}, {"cross", PAD_CROSS}, {"circle", PAD_CIRCLE},
                {"square", PAD_SQUARE}, {"triangle", PAD_TRIANGLE}, {"up", PAD_UP}, {"down", PAD_DOWN},
                {"left", PAD_LEFT}, {"right", PAD_RIGHT}, {"l1", PAD_L1}, {"r1", PAD_R1}, {"l2", PAD_L2}, {"r2", PAD_R2}};
            std::string spec(env);
            size_t pos = 0;
            while (pos < spec.size())
            {
                size_t end = spec.find(',', pos);
                if (end == std::string::npos)
                    end = spec.size();
                const std::string item = spec.substr(pos, end - pos);
                pos = end + 1;
                const size_t at = item.find('@');
                if (at == std::string::npos)
                    continue;
                const std::string name = item.substr(0, at);
                const std::string times = item.substr(at + 1);
                const size_t dash = times.find('-');
                const double from = std::atof(times.c_str());
                const double to = dash == std::string::npos ? from + 0.3 : std::atof(times.c_str() + dash + 1);
                for (const auto &n : names)
                    if (name == n.name)
                        out.push_back({n.bit, from, to});
                const char *axes[] = {"rx", "ry", "lx", "ly"};
                for (int a = 0; a < 4; ++a)
                    if (name.size() == 3 && name.compare(0, 2, axes[a]) == 0 && (name[2] == '-' || name[2] == '+'))
                        out.push_back({0, from, to, a, static_cast<uint8_t>(name[2] == '-' ? 0x00 : 0xFF)});
            }
            return out;
        }();
        return presses;
    }
    // Seconds since the first host pad sample: the clock of PS2X_AUTOPAD and of the [pad] log.
    double padClock()
    {
        static const auto start = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }

    uint16_t autoPressMask(uint8_t *axes)
    {
        const auto &presses = autoPresses();
        if (presses.empty())
            return 0;
        static const bool guestClock = [] { const char *e = std::getenv("PS2X_AUTOPAD_CLOCK"); return e && std::strcmp(e, "vblank") == 0; }();
        const double t = guestClock ? static_cast<double>(g_ps2xVsyncTickCount.load(std::memory_order_relaxed)) / 60.0 : padClock();
        uint16_t mask = 0;
        for (const auto &p : presses)
            if (t >= p.from && t < p.to)
            {
                mask |= p.bit;
                if (p.axis >= 0 && axes)
                    axes[p.axis] = p.value;
            }
        return mask;
    }

    // Mouse (keyboard mode): a click in the window captures the pointer (Alt, F1 or leaving the
    // window frees it). While captured, moving the mouse sideways tilts the right stick sideways
    // (turns the camera) by how fast it moves; the wheel = right stick up/down for a moment per
    // notch (camera zoom). Left click = Cross, right click = Triangle (cancel), middle click =
    // Circle (snap to Sim). The left stick (walking)
    // stays on WASD. Computed on the host thread once per frame, read by the pad reads.
    std::atomic<bool> g_mouseCaptured{false};
    std::atomic<uint32_t> g_mouseAxes{0x80808080u}; // stick bytes rx, ry, lx, ly
    std::atomic<uint16_t> g_mouseButtons{0};        // active-high PAD_* bits

    uint8_t stickByte(float v)
    {
        // The game ignores small tilts; start just past its dead zone so slow moves still count.
        constexpr float kDeadZone = 0.3f;
        if (std::fabs(v) < 0.02f)
            return kPadStickCenter;
        const float m = std::min(1.0f, kDeadZone + (1.0f - kDeadZone) * std::fabs(v));
        return static_cast<uint8_t>(std::lround(128.0f + (v < 0.0f ? -128.0f : 127.0f) * m));
    }

    void pollMouse(bool keyboardActive)
    {
        using clock = std::chrono::steady_clock;
        static float vx = 0.0f;
        static bool swallowLeft = false; // the click that captured the pointer
        static clock::time_point wheelUntil{};
        static float wheelDir = 0.0f;
        const auto now = clock::now();

        bool captured = g_mouseCaptured.load(std::memory_order_relaxed);
        if (captured && (!keyboardActive || !IsWindowFocused() || IsKeyPressed(KEY_LEFT_ALT) ||
                         IsKeyPressed(KEY_RIGHT_ALT) || IsKeyPressed(KEY_F1) || IsKeyPressed(KEY_F2)))
        {
            EnableCursor();
            captured = false;
        }
        else if (!captured && keyboardActive && IsWindowFocused() && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
                 !ps2x::mods::uiCapturesMouse()) // a click on a debug panel or Mods menu window
        {
            DisableCursor();
            captured = true;
            swallowLeft = true;
        }
        g_mouseCaptured.store(captured, std::memory_order_relaxed);
        if (!captured)
        {
            vx = 0.0f;
            g_mouseAxes.store(0x80808080u, std::memory_order_relaxed);
            g_mouseButtons.store(0, std::memory_order_relaxed);
            return;
        }

        // Stick tilt follows mouse speed (full tilt at ~12 px per host frame), smoothed a little.
        constexpr float kGain = 1.0f / 12.0f;
        const Vector2 delta = GetMouseDelta();
        vx += (std::clamp(delta.x * kGain, -1.0f, 1.0f) - vx) * 0.5f;

        const float wheel = GetMouseWheelMove();
        if (wheel != 0.0f)
        {
            wheelDir = wheel > 0.0f ? -1.0f : 1.0f; // wheel up = stick up
            wheelUntil = now + std::chrono::milliseconds(std::min(600, static_cast<int>(150 * std::fabs(wheel))));
        }

        const uint8_t rx = stickByte(-vx); // the game turns the view the other way from the stick
        uint8_t ry = kPadStickCenter;
        const uint8_t lx = kPadStickCenter, ly = kPadStickCenter;
        if (now < wheelUntil)
            ry = stickByte(wheelDir);
        g_mouseAxes.store(static_cast<uint32_t>(rx) | (static_cast<uint32_t>(ry) << 8) |
                              (static_cast<uint32_t>(lx) << 16) | (static_cast<uint32_t>(ly) << 24),
                          std::memory_order_relaxed);

        if (swallowLeft && !IsMouseButtonDown(MOUSE_BUTTON_LEFT))
            swallowLeft = false;
        uint16_t buttons = 0;
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) && !swallowLeft)
            buttons |= PAD_CROSS;
        if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT))
            buttons |= PAD_TRIANGLE;
        if (IsMouseButtonDown(MOUSE_BUTTON_MIDDLE))
            buttons |= PAD_CIRCLE;
        g_mouseButtons.store(buttons, std::memory_order_relaxed);
    }

    // Controllers, sampled by the host loop once per frame (pollPads) and read by the pad reads: every
    // connected pad drives port 0 (buttons OR'd, each stick axis from the pad pushing it furthest).
    std::atomic<uint16_t> g_padButtons{0};        // active-high PAD_* bits
    std::atomic<uint32_t> g_padAxes{0x80808080u}; // stick bytes rx, ry, lx, ly
    // Keyboard and mouse are ignored for a moment after pad input: Steam's desktop layout turns pad
    // presses into keys and clicks (A = Enter = Start), which would press buttons twice and flip the icons.
    std::atomic<bool> g_kbmEcho{false};
    constexpr auto kKbmEchoWindow = std::chrono::milliseconds(300);

    enum class InputDevice { Keyboard, Xbox, PlayStation };

    // PS2X_GLYPHS=keyboard|xbox|playstation pins the button icons; otherwise they follow the device last
    // used: keys or mouse clicks -> keycaps, a pad -> its own labels (PlayStation pads keep the game's
    // icons, every other pad gets Xbox ones).
    int forcedGlyphs()
    {
        static const int forced = []
        {
            const char *e = std::getenv("PS2X_GLYPHS");
            const std::string v = e ? e : "";
            if (v == "keyboard" || v == "kbm")
                return static_cast<int>(InputDevice::Keyboard);
            if (v == "xbox")
                return static_cast<int>(InputDevice::Xbox);
            if (v == "playstation" || v == "ps")
                return static_cast<int>(InputDevice::PlayStation);
            return -1;
        }();
        return forced;
    }

    bool kbmPressedThisFrame()
    {
        // Only the keys the game uses: debug keys (F-keys) don't change the icons.
        static const int keys[] = {KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_SPACE, KEY_BACKSPACE, KEY_R, KEY_F,
                                   KEY_Q, KEY_E, KEY_Z, KEY_C, KEY_LEFT_SHIFT, KEY_LEFT_CONTROL, KEY_ENTER,
                                   KEY_KP_ENTER, KEY_ESCAPE, KEY_TAB, KEY_W, KEY_A, KEY_S, KEY_D, KEY_I,
                                   KEY_J, KEY_K, KEY_L};
        for (int k : keys)
            if (IsKeyPressed(k))
                return true;
        return IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) ||
               IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE) || GetMouseWheelMove() != 0.0f;
    }

    // Vibration (scePadSetActDirect on the game thread -> pollPads on the host thread): bit 8 = the small
    // motor on, low byte = the large motor's speed. Only port 0 has pads. PS2X_RUMBLE=0 turns it off.
    std::atomic<uint16_t> g_rumble{0};

    bool rumbleEnabled()
    {
        static const bool enabled = [] { const char *e = std::getenv("PS2X_RUMBLE"); return !(e && *e == '0'); }();
        return enabled;
    }

    struct PadSample
    {
        uint16_t buttons = 0; // active-high PAD_* bits
        uint8_t axes[4] = {kPadStickCenter, kPadStickCenter, kPadStickCenter, kPadStickCenter};
    };

    // Folds one pad into the merged state; true when it was used this frame (a new press or a stick
    // pushed past half way), which makes it the input device.
    bool mergePad(PadSample &merged, const PadSample &pad, uint16_t &lastButtons)
    {
        merged.buttons |= pad.buttons;
        bool pushed = false;
        for (int a = 0; a < 4; ++a)
        {
            const int d = std::abs(pad.axes[a] - kPadStickCenter);
            if (d > std::abs(merged.axes[a] - kPadStickCenter))
                merged.axes[a] = pad.axes[a];
            pushed |= d > 0x40;
        }
        const bool pressed = (pad.buttons & ~lastButtons) != 0;
        lastButtons = pad.buttons;
        return pressed || pushed;
    }

#if defined(PS2X_SDL_PAD)
    // SDL2 reads XInput pads, DualShock 3/4, DualSense, Switch Pro / Joy-Cons and most DirectInput pads
    // (its built-in mapping list) directly, so no x360ce / DS4Windows is needed.
    struct HostPad
    {
        SDL_GameController *pad = nullptr;
        SDL_JoystickID id = -1;
        bool playstation = false;
        bool steamVirtual = false; // Steam Input's virtual pad: used only when no real pad can be read
        uint16_t lastButtons = 0;
    };
    std::vector<HostPad> g_hostPads;

    bool initSdlPads()
    {
        static const bool ok = []
        {
            // Games started by Steam get a list of devices to ignore (the pads Steam Input replaces with
            // its virtual one, laid out however Steam has it): drop it and read the real pads, unless
            // PS2X_STEAM_INPUT=1 asks for Steam's pad.
            const char *steam = std::getenv("PS2X_STEAM_INPUT");
            if (!(steam && *steam == '1'))
            {
                SDL_SetHintWithPriority(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES, "", SDL_HINT_OVERRIDE);
                SDL_SetHintWithPriority(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, "", SDL_HINT_OVERRIDE);
            }
            SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");               // Ctrl+C stays the console's
            SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1"); // no SDL window; focus is checked in pollPads
            SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0"); // Nintendo pads by position: bottom = Cross
            if (rumbleEnabled())
            {
                // DualShock 4 / DualSense over Bluetooth vibrate only in their extended report mode.
                SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
                SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
            }
            if (SDL_Init(SDL_INIT_GAMECONTROLLER) != 0)
            {
                std::fprintf(stderr, "[pad] no controller support: %s\n", SDL_GetError());
                return false;
            }
            // Mappings for unusual pads: gamecontrollerdb.txt (github.com/mdqinc/SDL_GameControllerDB) next to
            // the executable.
            if (char *base = SDL_GetBasePath())
            {
                const std::string path = std::string(base) + "gamecontrollerdb.txt";
                SDL_free(base);
                if (const int n = SDL_GameControllerAddMappingsFromFile(path.c_str()); n > 0)
                    std::fprintf(stderr, "[pad] %d controller mappings from %s\n", n, path.c_str());
            }
            return true;
        }();
        return ok;
    }

    void openSdlPad(int index)
    {
        if (!SDL_IsGameController(index))
            return;
        const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(index);
        for (const HostPad &p : g_hostPads)
            if (p.id == id)
                return;
        HostPad p;
        p.pad = SDL_GameControllerOpen(index);
        if (!p.pad)
            return;
        p.id = id;
        const SDL_GameControllerType type = SDL_GameControllerGetType(p.pad);
        p.playstation = type == SDL_CONTROLLER_TYPE_PS3 || type == SDL_CONTROLLER_TYPE_PS4 || type == SDL_CONTROLLER_TYPE_PS5;
        p.steamVirtual = SDL_GameControllerGetVendor(p.pad) == 0x28DE && SDL_GameControllerGetProduct(p.pad) == 0x11FF;
        const char *name = SDL_GameControllerName(p.pad);
        std::fprintf(stderr, "[pad] controller connected: %s (%s%s)\n", name ? name : "?",
                     p.playstation ? "PlayStation icons" : "Xbox icons", p.steamVirtual ? ", Steam virtual pad" : "");
        g_hostPads.push_back(p);
    }

    uint8_t axisByte(int v) // SDL axis -32768..32767 -> stick byte, 0x80 = centre
    {
        return static_cast<uint8_t>((std::clamp(v, -32768, 32767) + 32768) >> 8);
    }

    PadSample readSdlPad(SDL_GameController *c)
    {
        PadSample s;
        const struct { SDL_GameControllerButton b; uint16_t bit; } map[] = {
            {SDL_CONTROLLER_BUTTON_DPAD_UP, PAD_UP}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN, PAD_DOWN},
            {SDL_CONTROLLER_BUTTON_DPAD_LEFT, PAD_LEFT}, {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, PAD_RIGHT},
            {SDL_CONTROLLER_BUTTON_A, PAD_CROSS}, {SDL_CONTROLLER_BUTTON_B, PAD_CIRCLE},
            {SDL_CONTROLLER_BUTTON_X, PAD_SQUARE}, {SDL_CONTROLLER_BUTTON_Y, PAD_TRIANGLE},
            {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, PAD_L1}, {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, PAD_R1},
            {SDL_CONTROLLER_BUTTON_LEFTSTICK, PAD_L3}, {SDL_CONTROLLER_BUTTON_RIGHTSTICK, PAD_R3},
            {SDL_CONTROLLER_BUTTON_START, PAD_START}, {SDL_CONTROLLER_BUTTON_BACK, PAD_SELECT},
            {SDL_CONTROLLER_BUTTON_TOUCHPAD, PAD_SELECT}};
        for (const auto &m : map)
            if (SDL_GameControllerGetButton(c, m.b))
                s.buttons |= m.bit;
        // Analog triggers press L2/R2 past a third of the way.
        if (SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 10000)
            s.buttons |= PAD_L2;
        if (SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 10000)
            s.buttons |= PAD_R2;
        s.axes[0] = axisByte(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX));
        s.axes[1] = axisByte(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY));
        s.axes[2] = axisByte(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX));
        s.axes[3] = axisByte(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY));
        return s;
    }

    // Merges every pad into merged. usedDevice = the device of a pad used this frame, plugged = the device
    // of a pad connected this frame (-1 if none). Returns whether any pad is connected.
    bool pollPadDevices(PadSample &merged, int &usedDevice, int &plugged)
    {
        if (!initSdlPads())
            return false;
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            if (e.type == SDL_CONTROLLERDEVICEADDED)
            {
                const size_t before = g_hostPads.size();
                openSdlPad(e.cdevice.which);
                if (g_hostPads.size() != before && (!g_hostPads.back().steamVirtual || plugged < 0))
                    plugged = static_cast<int>(g_hostPads.back().playstation ? InputDevice::PlayStation : InputDevice::Xbox);
            }
            else if (e.type == SDL_CONTROLLERDEVICEREMOVED)
                for (size_t i = 0; i < g_hostPads.size(); ++i)
                    if (g_hostPads[i].id == e.cdevice.which)
                    {
                        const char *name = SDL_GameControllerName(g_hostPads[i].pad);
                        std::fprintf(stderr, "[pad] controller disconnected: %s\n", name ? name : "?");
                        SDL_GameControllerClose(g_hostPads[i].pad);
                        g_hostPads.erase(g_hostPads.begin() + static_cast<std::ptrdiff_t>(i));
                        break;
                    }
        }
        bool realPad = false;
        for (const HostPad &p : g_hostPads)
            realPad |= !p.steamVirtual;
        for (HostPad &p : g_hostPads)
        {
            if (p.steamVirtual && realPad)
                continue; // the same controller, already read directly
            // A PlayStation pad used in the same frame as another wins the icons (DS4Windows' virtual Xbox pad).
            if (mergePad(merged, readSdlPad(p.pad), p.lastButtons) && (usedDevice < 0 || p.playstation))
                usedDevice = static_cast<int>(p.playstation ? InputDevice::PlayStation : InputDevice::Xbox);
        }
        return !g_hostPads.empty();
    }

    // Drives the motors of every pad that plays: the large motor is the low-frequency one, the small
    // motor a fixed high-frequency buzz. Each call lasts 150 ms and is renewed every frame while the game
    // keeps it on (SDL only sends to the pad when the strength changes), so a stalled game stops it.
    void applyRumble(uint16_t motors)
    {
        static uint16_t applied = 0;
        if (motors == 0 && applied == 0)
            return;
        const Uint16 low = static_cast<Uint16>((motors & 0xFFu) * 257u);
        const Uint16 high = (motors & 0x100u) ? 0xC000u : 0u;
        bool realPad = false;
        for (const HostPad &p : g_hostPads)
            realPad |= !p.steamVirtual;
        for (const HostPad &p : g_hostPads)
            if (!(p.steamVirtual && realPad))
                SDL_GameControllerRumble(p.pad, low, high, motors ? 150u : 0u);
        static bool logged = false;
        if (motors != 0 && !logged)
        {
            logged = true;
            std::fprintf(stderr, "[pad] rumble: the game started vibration (large=%u small=%d) on %zu controller(s)\n",
                         motors & 0xFFu, (motors & 0x100u) ? 1 : 0, g_hostPads.size());
        }
        applied = motors;
    }
#else
    // Vita / Android: raylib's gamepad 0.
    bool pollPadDevices(PadSample &merged, int &usedDevice, int &plugged)
    {
        constexpr int kGamepad = 0;
        static bool wasAvailable = false;
        const bool available = IsGamepadAvailable(kGamepad);
        if (available && !wasAvailable)
            plugged = static_cast<int>(InputDevice::PlayStation);
        wasAvailable = available;
        if (!available)
            return false;
        PadSample s;
        const struct { int b; uint16_t bit; } map[] = {
            {GAMEPAD_BUTTON_LEFT_FACE_UP, PAD_UP}, {GAMEPAD_BUTTON_LEFT_FACE_DOWN, PAD_DOWN},
            {GAMEPAD_BUTTON_LEFT_FACE_LEFT, PAD_LEFT}, {GAMEPAD_BUTTON_LEFT_FACE_RIGHT, PAD_RIGHT},
            {GAMEPAD_BUTTON_RIGHT_FACE_DOWN, PAD_CROSS}, {GAMEPAD_BUTTON_RIGHT_FACE_RIGHT, PAD_CIRCLE},
            {GAMEPAD_BUTTON_RIGHT_FACE_LEFT, PAD_SQUARE}, {GAMEPAD_BUTTON_RIGHT_FACE_UP, PAD_TRIANGLE},
            {GAMEPAD_BUTTON_LEFT_TRIGGER_1, PAD_L1}, {GAMEPAD_BUTTON_RIGHT_TRIGGER_1, PAD_R1},
            {GAMEPAD_BUTTON_LEFT_TRIGGER_2, PAD_L2}, {GAMEPAD_BUTTON_RIGHT_TRIGGER_2, PAD_R2},
            {GAMEPAD_BUTTON_MIDDLE_RIGHT, PAD_START}, {GAMEPAD_BUTTON_MIDDLE_LEFT, PAD_SELECT},
            {GAMEPAD_BUTTON_LEFT_THUMB, PAD_L3}, {GAMEPAD_BUTTON_RIGHT_THUMB, PAD_R3}};
        for (const auto &m : map)
            if (IsGamepadButtonDown(kGamepad, m.b))
                s.buttons |= m.bit;
        const int axes[4] = {GAMEPAD_AXIS_RIGHT_X, GAMEPAD_AXIS_RIGHT_Y, GAMEPAD_AXIS_LEFT_X, GAMEPAD_AXIS_LEFT_Y};
        for (int a = 0; a < 4; ++a)
            s.axes[a] = static_cast<uint8_t>(128 + GetGamepadAxisMovement(kGamepad, axes[a]) * 127);
        static uint16_t lastButtons = 0;
        if (mergePad(merged, s, lastButtons))
            usedDevice = static_cast<int>(InputDevice::PlayStation);
        return true;
    }
#endif

    // The device in use: a pad found at start or plugged in, then whichever was used last.
    InputDevice g_inputDevice = InputDevice::Keyboard;

    void pollPads(bool focused)
    {
        static std::chrono::steady_clock::time_point lastPadUse{};
        const auto now = std::chrono::steady_clock::now();
        PadSample merged;
        int usedDevice = -1, plugged = -1;
        const bool anyPad = pollPadDevices(merged, usedDevice, plugged);
        if (!focused)
        {
            merged = PadSample{}; // pads don't play the game from the background
            usedDevice = -1;
        }
        const bool padHeld = merged.buttons != 0 || std::any_of(std::begin(merged.axes), std::end(merged.axes),
                                                                [](uint8_t v) { return std::abs(v - kPadStickCenter) > 0x40; });
        if (padHeld || usedDevice >= 0)
            lastPadUse = now;
        const bool echo = anyPad && now - lastPadUse < kKbmEchoWindow;
        g_kbmEcho.store(echo, std::memory_order_relaxed);
        g_padButtons.store(merged.buttons, std::memory_order_relaxed);
        g_padAxes.store(static_cast<uint32_t>(merged.axes[0]) | (static_cast<uint32_t>(merged.axes[1]) << 8) |
                            (static_cast<uint32_t>(merged.axes[2]) << 16) | (static_cast<uint32_t>(merged.axes[3]) << 24),
                        std::memory_order_relaxed);

        InputDevice next = g_inputDevice;
        if (plugged >= 0)
            next = static_cast<InputDevice>(plugged);
        if (usedDevice >= 0)
            next = static_cast<InputDevice>(usedDevice);
        else if (focused && !echo && kbmPressedThisFrame())
            next = InputDevice::Keyboard;
        if (!anyPad)
            next = InputDevice::Keyboard;
        if (next != g_inputDevice)
        {
            static const char *const names[] = {"keyboard and mouse", "controller (Xbox icons)", "controller (PlayStation icons)"};
            std::fprintf(stderr, "[pad] input: %s\n", names[static_cast<int>(next)]);
            g_inputDevice = next;
        }

#if defined(PS2X_SDL_PAD)
        // No vibration from the background, while the keyboard plays, or with PS2X_RUMBLE=0.
        const bool rumbleOn = rumbleEnabled() && focused && g_inputDevice != InputDevice::Keyboard;
        applyRumble(rumbleOn ? g_rumble.load(std::memory_order_relaxed) : 0);
#endif
    }

    // Buttons (active-high) seen pressed by the host loop since the game last read port 0.
    // The game reads the pad once per guest frame; at a few FPS a quick tap would otherwise
    // start and end between two reads and never be seen.
    std::atomic<uint16_t> g_latchedPressed{0};

    // While a runtime menu (the save slot picker) has the input, the game sees an idle pad; the block
    // stays until every button is let go, so the press that closed the menu doesn't reach the game.
    std::atomic<bool> g_padBlocked{false};
    std::atomic<bool> g_padUnblockOnRelease{false};
    std::atomic<uint16_t> g_hostPressed{0};
}

// Current host buttons as the PS2 active-low mask; fills the stick bytes when axes is set.
static uint16_t sampleHostButtons(uint8_t *axes);

// Whether the keyboard and mouse are the input (the mouse is captured only then).
std::atomic<bool> g_ps2xPadKeyboardActive{true};
// The button icons shown (gs_texture_replace.cpp): 0 the game's PlayStation icons, 1 keycaps, 2 Xbox.
std::atomic<int> g_ps2xGlyphSet{1};

// Called by the host loop every frame (declared extern in ps2_runtime.cpp).
// Every press as a PS2X_AUTOPAD item ("[pad] cross@48.20-48.45", stick pushes as lx-/ly+...), so a
// played route can be replayed headless. On by default (one line per press); PS2X_PAD_LOG=0 = off.
static void logPadPresses(uint16_t pressed, const uint8_t axes[4])
{
    static const bool enabled = [] { const char *e = std::getenv("PS2X_PAD_LOG"); return !(e && *e == '0'); }();
    if (!enabled)
        return;
    static const char *const buttonNames[16] = {"select", "l3", "r3", "start", "up", "right", "down", "left",
                                                "l2", "r2", "l1", "r1", "triangle", "circle", "cross", "square"};
    static const char *const stickNames[8] = {"rx-", "rx+", "ry-", "ry+", "lx-", "lx+", "ly-", "ly+"};
    static double downSince[24] = {};
    static uint32_t held = 0;
    uint32_t now = pressed;
    for (int a = 0; a < 4; ++a)
    {
        if (axes[a] < 0x40)
            now |= 1u << (16 + 2 * a);
        else if (axes[a] > 0xC0)
            now |= 1u << (17 + 2 * a);
    }
    if (now == held)
        return;
    const double t = padClock();
    for (int i = 0; i < 24; ++i)
    {
        const uint32_t bit = 1u << i;
        if ((now & bit) && !(held & bit))
            downSince[i] = t;
        else if (!(now & bit) && (held & bit))
            std::fprintf(stderr, "[pad] %s@%.2f-%.2f\n", i < 16 ? buttonNames[i] : stickNames[i - 16], downSince[i], t);
    }
    held = now;
}

void ps2PadPollHost()
{
    padClock(); // starts the pad clock at the first host frame
    pollPads(IsWindowFocused());
    const bool keyboardActive = g_inputDevice == InputDevice::Keyboard;
    g_ps2xPadKeyboardActive.store(keyboardActive, std::memory_order_relaxed);
    const int forced = forcedGlyphs();
    const InputDevice shown = forced >= 0 ? static_cast<InputDevice>(forced) : g_inputDevice;
    g_ps2xGlyphSet.store(shown == InputDevice::PlayStation ? 0 : shown == InputDevice::Keyboard ? 1 : 2, std::memory_order_relaxed);
    pollMouse(keyboardActive);
    uint8_t axes[4] = {kPadStickCenter, kPadStickCenter, kPadStickCenter, kPadStickCenter};
    const uint16_t pressed = static_cast<uint16_t>(~sampleHostButtons(axes));
    logPadPresses(pressed, axes);
    g_hostPressed.store(pressed, std::memory_order_relaxed);
    if (g_padUnblockOnRelease.load(std::memory_order_relaxed) && pressed == 0u)
    {
        g_padUnblockOnRelease.store(false, std::memory_order_relaxed);
        g_padBlocked.store(false, std::memory_order_relaxed);
    }
    if (pressed != 0u)
        g_latchedPressed.fetch_or(pressed, std::memory_order_relaxed);
}

void ps2PadSetBlocked(bool blocked)
{
    if (blocked)
    {
        g_padUnblockOnRelease.store(false, std::memory_order_relaxed);
        g_padBlocked.store(true, std::memory_order_relaxed);
    }
    else
    {
        g_padUnblockOnRelease.store(true, std::memory_order_relaxed);
    }
}

uint16_t ps2PadHostPressed()
{
    return g_hostPressed.load(std::memory_order_relaxed);
}

// Motor values from scePadSetActDirect (Kernel/Stubs/Pad.cpp, game thread); pollPads plays them.
void ps2PadSetRumble(int port, bool small, uint8_t large)
{
    if (port != 0)
        return;
    g_rumble.store(static_cast<uint16_t>((small ? 0x100u : 0u) | large), std::memory_order_relaxed);
}

// Window title hint about the mouse (main.cpp).
const char *ps2PadMouseHint()
{
    if (!g_ps2xPadKeyboardActive.load(std::memory_order_relaxed))
        return "";
    return g_mouseCaptured.load(std::memory_order_relaxed) ? "Alt frees the mouse" : "click to use the mouse";
}

bool PSPadBackend::readState(int port, int slot, uint8_t *data, size_t size)
{
    if (!data || size < 32)
        return false;

    std::memset(data, 0, 32);
    data[0] = 0x01;
    data[1] = kPadAnalogMarker;
    data[2] = 0xFF;
    data[3] = 0xFF;
    data[4] = data[5] = data[6] = data[7] = kPadStickCenter;

    if (ps2NetplayActive())
    {
        // Online co-op: the host is port 0, the joiner port 1. The port 0 read exchanges this
        // machine's pad with the peer's; port 1 reads in the same frame reuse that exchange.
        static uint16_t netButtons[2] = {0xFFFFu, 0xFFFFu};
        static uint8_t netAxes[2][4] = {{kPadStickCenter, kPadStickCenter, kPadStickCenter, kPadStickCenter},
                                        {kPadStickCenter, kPadStickCenter, kPadStickCenter, kPadStickCenter}};
        if (slot != 0)
            return true;
        if (port == 0)
        {
            uint8_t axes[4] = {kPadStickCenter, kPadStickCenter, kPadStickCenter, kPadStickCenter};
            uint16_t local = 0xFFFFu;
            if (!g_padBlocked.load(std::memory_order_relaxed))
            {
                local = sampleHostButtons(axes);
                local &= static_cast<uint16_t>(~g_latchedPressed.exchange(0, std::memory_order_relaxed));
            }
            else
                g_latchedPressed.store(0, std::memory_order_relaxed);
            ps2NetplayExchange(local, axes, netButtons, netAxes);
        }
        const int p = port & 1;
        data[2] = static_cast<uint8_t>(netButtons[p] & 0xFF);
        data[3] = static_cast<uint8_t>(netButtons[p] >> 8);
        std::memcpy(data + 4, netAxes[p], 4);
        return true;
    }

    if (g_padBlocked.load(std::memory_order_relaxed))
    {
        g_latchedPressed.store(0, std::memory_order_relaxed);
        return true;
    }
    uint16_t btns = sampleHostButtons(data + 4);
    if (port == 0 && slot == 0)
        btns &= static_cast<uint16_t>(~g_latchedPressed.exchange(0, std::memory_order_relaxed));
    data[2] = static_cast<uint8_t>(btns & 0xFF);
    data[3] = static_cast<uint8_t>(btns >> 8);
    return true;
}

static uint16_t sampleHostButtons(uint8_t *axes)
{
    uint16_t btns = static_cast<uint16_t>(~g_padButtons.load(std::memory_order_relaxed));
    if (axes)
    {
        const uint32_t pad = g_padAxes.load(std::memory_order_relaxed);
        for (int a = 0; a < 4; ++a)
            axes[a] = static_cast<uint8_t>(pad >> (8 * a));
    }
    auto clearBit = [&btns](uint16_t mask)
    { btns &= ~mask; };

    if (!g_kbmEcho.load(std::memory_order_relaxed) && !ps2x::mods::uiCapturesKeyboard()) // not while typing in a UI box
    {
        // Keyboard (the in-game button icons are swapped for these keys, see texpacks/kbm):
        // left stick WASD, right stick IJKL, d-pad arrows, Cross Space, Circle Backspace,
        // Square R, Triangle F, L1/R1 Q/E, L2/R2 Z/C, L3/R3 Left Shift/Left Ctrl, Start Enter/Esc (pause),
        // Select Tab. Works alongside the pads.
        if (IsKeyDown(KEY_UP))
            clearBit(PAD_UP);
        if (IsKeyDown(KEY_DOWN))
            clearBit(PAD_DOWN);
        if (IsKeyDown(KEY_LEFT))
            clearBit(PAD_LEFT);
        if (IsKeyDown(KEY_RIGHT))
            clearBit(PAD_RIGHT);
        if (IsKeyDown(KEY_SPACE))
            clearBit(PAD_CROSS);
        if (IsKeyDown(KEY_BACKSPACE))
            clearBit(PAD_CIRCLE);
        if (IsKeyDown(KEY_R))
            clearBit(PAD_SQUARE);
        if (IsKeyDown(KEY_F))
            clearBit(PAD_TRIANGLE);
        if (IsKeyDown(KEY_Q))
            clearBit(PAD_L1);
        if (IsKeyDown(KEY_E))
            clearBit(PAD_R1);
        if (IsKeyDown(KEY_Z))
            clearBit(PAD_L2);
        if (IsKeyDown(KEY_C))
            clearBit(PAD_R2);
        if (IsKeyDown(KEY_LEFT_SHIFT))
            clearBit(PAD_L3);
        if (IsKeyDown(KEY_LEFT_CONTROL))
            clearBit(PAD_R3);
        if (IsKeyDown(KEY_ENTER) || IsKeyDown(KEY_KP_ENTER) || IsKeyDown(KEY_ESCAPE))
            clearBit(PAD_START);
        if (IsKeyDown(KEY_TAB))
            clearBit(PAD_SELECT);

        btns &= static_cast<uint16_t>(~g_mouseButtons.load(std::memory_order_relaxed));

        if (axes)
        {
            // Keys win over the pads and the mouse on each axis; the mouse only moves an axis no pad pushes.
            const uint32_t mouse = g_mouseAxes.load(std::memory_order_relaxed);
            auto axis = [mouse, axes](int index, int negative, int positive)
            {
                if (IsKeyDown(negative) != IsKeyDown(positive))
                    return static_cast<uint8_t>(IsKeyDown(positive) ? 0xFF : 0x00);
                if (axes[index] != kPadStickCenter)
                    return axes[index];
                return static_cast<uint8_t>(mouse >> (8 * index));
            };
            axes[0] = axis(0, KEY_J, KEY_L);
            axes[1] = axis(1, KEY_I, KEY_K);
            axes[2] = axis(2, KEY_A, KEY_D);
            axes[3] = axis(3, KEY_W, KEY_S);
        }
    }

    btns &= static_cast<uint16_t>(~autoPressMask(axes));
    return btns;
}
