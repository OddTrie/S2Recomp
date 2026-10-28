#ifndef PS2_NETPLAY_H
#define PS2_NETPLAY_H

#include <cstdint>
#include <string>

// Online co-op through Epic Online Services P2P (ps2_netplay.cpp), lockstep on pad input:
// PS2X_NETPLAY=host prints a join code and plays port 0, PS2X_NETPLAY=join with PS2X_NETPLAY_CODE=<code>
// plays port 1. Both run the same build from boot; each frame's input is exchanged before the game sees it.

// True when PS2X_NETPLAY is set (and the build has the EOS SDK).
bool ps2NetplayActive();

// 0 when hosting, 1 when joined, -1 outside co-op.
int ps2NetplayLocalPlayer();

// Called on the game thread before the game starts: the host snapshots its memory cards to send,
// the joiner waits for that copy and boots from it (<exe>/netplay_mc), so both games load the same save.
void ps2NetplayShareSave();

// Called on every port 0 pad read (one per game frame): hands over this machine's input
// (active-low buttons, axes rx ry lx ly) and blocks until both players' input for the frame is known.
void ps2NetplayExchange(uint16_t localButtons, const uint8_t localAxes[4],
                        uint16_t outButtons[2], uint8_t outAxes[2][4]);

// Shared co-op clock, so the game reads the same time on both machines: frame = game frames so far,
// localEpoch = the host's local date/time at boot + frame / 60, as seconds since 1970 (read it with gmtime).
// False outside co-op or before the host's clock arrived.
bool ps2NetplayClock(int64_t &frame, int64_t &localEpoch);

// PS2X_NETPLAY_HASH=N: every N frames hash RDRAM and compare with the peer (logs a desync).
void ps2NetplayCheckState(const uint8_t *rdram);

// Text to show over the picture while the game waits for the other player (empty otherwise).
std::string ps2NetplayOverlayText();

void ps2NetplayShutdown();

#endif
