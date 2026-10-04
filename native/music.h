#pragma once

#include <stdint.h>

enum MusicState {
    MUSIC_STOPPED = 0,
    MUSIC_STARTING,
    MUSIC_PLAYING,
    MUSIC_FAILED
};

struct MusicSnapshot {
    int state;
    int track;
    int volume;
    bool muted;
    int32_t errorCode;
};

// Call control functions from the UI thread. Start validates small WAV headers
// and retains both files before returning; all PCM/audio I/O stays on a worker.
// A successful start never resolves /app0 paths again during playback, including
// while the installer temporarily changes the process root.
// The profile ID is retained in the API for callers, but MAIN audio always uses
// the native SYSTEM user (0xFF); signed-in profiles remain for controller input.
bool musicStart(int32_t userId);
void musicSetVolume(int percent);
void musicToggleMute();
void musicNextTrack();
void musicStop(); // Requests a stop; never waits for an audio buffer on the UI.
void musicShutdown(); // Joins only during application teardown.
MusicSnapshot musicSnapshot();
const char* musicTrackName(int track);
