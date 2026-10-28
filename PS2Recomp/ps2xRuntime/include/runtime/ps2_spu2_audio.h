#pragma once

// Host output for the IOP's SPU2 mixer (ps2x/iop/spu2_audio.h): a raylib AudioStream fed from a
// small ring buffer (time-stretched when emulation runs slower than real time), plus an optional WAV dump.
//   PS2X_AUDIO=0              no sound output (without a dump the SPU2 then only runs its voice clock)
//   PS2X_AUDIO_DUMP=<file>    write the mixed 48 kHz stereo output to a WAV file (works with PS2X_AUDIO=0)
//   PS2X_AUDIO_DEVICE_DUMP=<file>  what the device plays, in real time (logs the wall-clock start; for screen recordings)
//   PS2X_AUDIO_VOLUME=<0-100>  speaker volume (default 50; dumps keep the full level)
//   PS2X_AUDIO_LATENCY=<ms>   target output buffering (default 80; below it the output is time-stretched)
namespace ps2x_audio
{
    // Call after InitAudioDevice(); `deviceReady` = IsAudioDeviceReady().
    void startSpu2Output(bool deviceReady);
    // Call before CloseAudioDevice().
    void stopSpu2Output();
}
