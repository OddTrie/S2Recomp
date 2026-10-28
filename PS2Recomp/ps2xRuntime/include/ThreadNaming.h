#pragma once

#include <string_view>
#include <string>
#include <cstdlib>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WINAPI
#define WINAPI __stdcall
#endif

extern "C"
{
    typedef void *HANDLE;
    typedef void *HMODULE;
    typedef const wchar_t *PCWSTR;
    typedef long HRESULT;

    __declspec(dllimport) HMODULE WINAPI GetModuleHandleW(const wchar_t *lpModuleName);
    __declspec(dllimport) void *WINAPI GetProcAddress(HMODULE hModule, const char *lpProcName);
    __declspec(dllimport) HANDLE WINAPI GetCurrentThread(void);
}
#elif defined(__APPLE__) || defined(__linux__)
#include <pthread.h>
#endif

namespace ThreadNaming
{
    inline void SetCurrentThreadName(std::string_view name)
    {
#if defined(_WIN32) 
        using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);

        HMODULE kernel32 = ::GetModuleHandleW(L"Kernel32.dll");
        auto setThreadDescription =
            reinterpret_cast<SetThreadDescriptionFn>(::GetProcAddress(kernel32, "SetThreadDescription"));

        if (setThreadDescription)
        {
            std::wstring wname(name.begin(), name.end());
            setThreadDescription(::GetCurrentThread(), wname.c_str());
        }

#elif defined(__APPLE__)
        pthread_setname_np(name.data());

#elif defined(__linux__)
        pthread_setname_np(pthread_self(), name.data());
#endif
    }

    // For the thread that limits the frame rate (VU1 worker): higher priority and no power throttling
    // (EcoQoS), so Windows 11 keeps it on a performance core of a hybrid CPU instead of an E-core.
    // Only timing changes. PS2X_THREAD_BOOST=0 turns it off (A/B).
    inline void BoostCurrentThread()
    {
#if defined(_WIN32)
        const char *env = std::getenv("PS2X_THREAD_BOOST");
        if (env && *env == '0')
            return;
        using SetThreadPriorityFn = int(WINAPI *)(HANDLE, int);
        using SetThreadInformationFn = int(WINAPI *)(HANDLE, int, void *, unsigned long);
        HMODULE kernel32 = ::GetModuleHandleW(L"Kernel32.dll");
        if (auto setPriority = reinterpret_cast<SetThreadPriorityFn>(::GetProcAddress(kernel32, "SetThreadPriority")))
            setPriority(::GetCurrentThread(), 2 /* THREAD_PRIORITY_HIGHEST */);
        if (auto setInformation = reinterpret_cast<SetThreadInformationFn>(::GetProcAddress(kernel32, "SetThreadInformation")))
        {
            // THREAD_POWER_THROTTLING_STATE {Version 1, ControlMask EXECUTION_SPEED, StateMask 0 = never throttle}
            unsigned long state[3] = {1u, 1u, 0u};
            setInformation(::GetCurrentThread(), 3 /* ThreadPowerThrottling */, state, sizeof(state));
        }
#endif
    }
}
