#include "music.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <orbis/AudioOut.h>
#include <orbis/Sysmodule.h>
#include <orbis/libkernel.h>

// OpenOrbis 0.5.4 leaves the native internal module probe declared void().
extern "C" int32_t peppyMusicModuleIsLoaded(OrbisSysModuleInternal)
    __asm__("sceSysmoduleIsLoadedInternal");

#ifndef PEPPY_MUSIC_DIRECTORY
#define PEPPY_MUSIC_DIRECTORY "/app0/assets/music"
#endif

namespace {
const unsigned FRAMES = 256;
const unsigned CHANNELS = 2;
const unsigned FRAME_BYTES = CHANNELS * sizeof(int16_t);
const uint64_t MAX_WAV_BYTES = 128ULL * 1024 * 1024;
const int32_t MUSIC_ERROR_FILE = -2100;
const int32_t MUSIC_ERROR_FORMAT = -2101;
const int32_t MUSIC_ERROR_READ = -2102;
const char* const TRACK_FILES[2] = {
    PEPPY_MUSIC_DIRECTORY "/fight.wav",
    PEPPY_MUSIC_DIRECTORY "/acendaofarol.wav"
};

int g_state = MUSIC_STOPPED, g_running = 0, g_stop = 0, g_skip = 0;
int g_track = 0, g_volume = 30, g_muted = 0;
int32_t g_error = 0;
OrbisPthread g_thread;
bool g_joinable = false;

struct WavStream {
    FILE* file;
    uint64_t remaining;
    uint64_t total;
    long dataOffset;
    WavStream() : file(0), remaining(0), total(0), dataOffset(0) {}
};
WavStream g_prepared[2];

uint16_t little16(const unsigned char* p) {
    return uint16_t(p[0]) | uint16_t(p[1]) << 8;
}
uint32_t little32(const unsigned char* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 |
           uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
void closeWav(WavStream& wav) {
    if (wav.file) fclose(wav.file);
    wav.file = 0;
    wav.remaining = 0;
    wav.total = 0;
    wav.dataOffset = 0;
}

// Only the PCM format emitted by prepare-music.py is accepted. Unknown RIFF
// chunks are skipped without allocating their declared length.
int32_t openWav(const char* path, WavStream& wav) {
    closeWav(wav);
    FILE* file = fopen(path, "rb");
    if (!file) return MUSIC_ERROR_FILE;
    unsigned char header[16];
    bool valid = false;
    uint64_t dataBytes = 0;
    do {
        if (fseek(file, 0, SEEK_END) != 0) break;
        long fileBytes = ftell(file);
        if (fileBytes < 44 || uint64_t(fileBytes) > MAX_WAV_BYTES) break;
        if (fseek(file, 0, SEEK_SET) != 0 || fread(header, 1, 12, file) != 12) break;
        if (memcmp(header, "RIFF", 4) || memcmp(header + 8, "WAVE", 4)) break;
        uint64_t riffEnd = uint64_t(little32(header + 4)) + 8;
        if (riffEnd > uint64_t(fileBytes) || riffEnd < 44) break;
        bool formatSeen = false;
        uint64_t position = 12;
        while (position + 8 <= riffEnd) {
            if (fread(header, 1, 8, file) != 8) break;
            uint64_t chunkBytes = little32(header + 4);
            position += 8;
            uint64_t padded = chunkBytes + (chunkBytes & 1);
            if (padded > riffEnd - position) break;
            if (!memcmp(header, "fmt ", 4)) {
                if (formatSeen || chunkBytes < 16 || fread(header, 1, 16, file) != 16) break;
                if (little16(header) != 1 || little16(header + 2) != CHANNELS ||
                    little32(header + 4) != 48000 || little32(header + 8) != 192000 ||
                    little16(header + 12) != FRAME_BYTES || little16(header + 14) != 16) break;
                formatSeen = true;
                if (fseek(file, long(padded - 16), SEEK_CUR) != 0) break;
            } else if (!memcmp(header, "data", 4)) {
                if (!formatSeen || !chunkBytes || chunkBytes % FRAME_BYTES) break;
                dataBytes = chunkBytes;
                valid = true;
                break; // The file position is the first PCM sample.
            } else if (fseek(file, long(padded), SEEK_CUR) != 0) break;
            position += padded;
        }
    } while (false);
    if (!valid) {
        fclose(file);
        return MUSIC_ERROR_FORMAT;
    }
    wav.file = file;
    wav.remaining = dataBytes;
    wav.total = dataBytes;
    wav.dataOffset = ftell(file);
    if (wav.dataOffset < 0) {
        closeWav(wav);
        return MUSIC_ERROR_READ;
    }
    return 0;
}

int32_t rewindWav(WavStream& wav) {
    if (!wav.file || fseek(wav.file, wav.dataOffset, SEEK_SET) != 0) return MUSIC_ERROR_READ;
    wav.remaining = wav.total;
    return 0;
}
void closePreparedTracks() {
    for (int i = 0; i < 2; ++i) closeWav(g_prepared[i]);
}

int32_t readFrames(WavStream& wav, int16_t* samples) {
    const size_t blockBytes = FRAMES * FRAME_BYTES;
    const size_t bytes = wav.remaining < blockBytes ? size_t(wav.remaining) : blockBytes;
    if (bytes && fread(samples, 1, bytes, wav.file) != bytes) return MUSIC_ERROR_READ;
    wav.remaining -= bytes;
    if (bytes < blockBytes) memset(reinterpret_cast<char*>(samples) + bytes, 0, blockBytes - bytes);
    return 0;
}

void applyGain(int16_t* samples, int volume, bool muted) {
    if (muted || volume == 0) {
        memset(samples, 0, FRAMES * FRAME_BYTES);
    } else if (volume != 100) {
        for (unsigned i = 0; i < FRAMES * CHANNELS; ++i)
            samples[i] = int16_t(int32_t(samples[i]) * volume / 100);
    }
}

bool stopping() { return __atomic_load_n(&g_stop, __ATOMIC_ACQUIRE) != 0; }
void failMusic(int32_t code) {
    __atomic_store_n(&g_error, code, __ATOMIC_RELEASE);
    __atomic_store_n(&g_state, MUSIC_FAILED, __ATOMIC_RELEASE);
}
int32_t loadAudioModule() {
    if (peppyMusicModuleIsLoaded(ORBIS_SYSMODULE_INTERNAL_AUDIOOUT) == 0) return 0;
    int32_t rc = int32_t(sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_AUDIOOUT));
    if (rc < 0 && peppyMusicModuleIsLoaded(ORBIS_SYSMODULE_INTERNAL_AUDIOOUT) != 0) return rc;
    return 0;
}

void* musicWorker(void*) {
    int32_t audio = -1;
    int32_t rc = loadAudioModule();
    alignas(16) int16_t samples[FRAMES * CHANNELS];
    do {
        if (rc < 0) { failMusic(rc); break; }
        if (stopping()) break;
        rc = sceAudioOutInit();
        if (rc != 0 && uint32_t(rc) != ORBIS_AUDIO_OUT_ERROR_ALREADY_INIT) { failMusic(rc); break; }
        // MAIN is the application's system mixer, not the signed-in profile's
        // device. Use the SYSTEM ID exactly as OpenOrbis' audio-wav example;
        // passing the controller profile can return INVALID_USER (0x809B0001).
        audio = sceAudioOutOpen(ORBIS_USER_SERVICE_USER_ID_SYSTEM, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, FRAMES, 48000,
                                ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
        if (audio <= 0) { failMusic(audio ? audio : MUSIC_ERROR_FILE); break; }
        int currentTrack = 0, attempted = 0;
        bool trackReady = false;
        while (!stopping()) {
            if (__atomic_exchange_n(&g_skip, 0, __ATOMIC_ACQ_REL)) {
                currentTrack = (currentTrack + 1) % 2;
                attempted = 0;
                trackReady = false;
            }
            WavStream& wav = g_prepared[currentTrack];
            if (!trackReady) {
                if (!wav.file) {
                    if (++attempted >= 2) { failMusic(MUSIC_ERROR_FILE); break; }
                    currentTrack = (currentTrack + 1) % 2;
                    continue;
                }
                rc = rewindWav(wav);
                if (rc != 0) { failMusic(rc); break; }
                attempted = 0;
                trackReady = true;
                __atomic_store_n(&g_track, currentTrack, __ATOMIC_RELEASE);
                __atomic_store_n(&g_state, MUSIC_PLAYING, __ATOMIC_RELEASE);
            }
            // Wait for the previous submission before overwriting this fixed
            // buffer. This blocking native call only executes on the worker.
            rc = sceAudioOutOutput(audio, 0);
            if (rc < 0) { failMusic(rc); break; }
            if (stopping()) break;
            rc = readFrames(wav, samples);
            if (rc != 0) { failMusic(rc); break; }
            applyGain(samples, __atomic_load_n(&g_volume, __ATOMIC_ACQUIRE),
                      __atomic_load_n(&g_muted, __ATOMIC_ACQUIRE) != 0);
            rc = sceAudioOutOutput(audio, samples);
            if (rc < 0) { failMusic(rc); break; }
            if (!wav.remaining) {
                currentTrack = (currentTrack + 1) % 2;
                trackReady = false;
            }
        }
    } while (false);
    closePreparedTracks();
    if (audio > 0) {
        // Finish any outstanding submission before its stack buffer disappears.
        sceAudioOutOutput(audio, 0);
        sceAudioOutClose(audio);
    }
    if (__atomic_load_n(&g_state, __ATOMIC_ACQUIRE) != MUSIC_FAILED)
        __atomic_store_n(&g_state, MUSIC_STOPPED, __ATOMIC_RELEASE);
    __atomic_store_n(&g_running, 0, __ATOMIC_RELEASE);
    return 0;
}
} // namespace

bool musicStart(int32_t) {
    if (__atomic_load_n(&g_running, __ATOMIC_ACQUIRE)) return false;
    if (g_joinable) musicShutdown();
    __atomic_store_n(&g_stop, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_skip, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_error, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_track, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_state, MUSIC_STARTING, __ATOMIC_RELEASE);
    // Pre-open both paths while the regular application root is still active.
    // The worker owns these descriptors after its creation and only seeks them;
    // changing process root during local installation cannot hide a next track.
    int32_t preparationError = MUSIC_ERROR_FILE;
    int preparedCount = 0;
    for (int i = 0; i < 2; ++i) {
        int32_t rc = openWav(TRACK_FILES[i], g_prepared[i]);
        if (rc == 0) ++preparedCount;
        else preparationError = rc;
    }
    if (!preparedCount) {
        closePreparedTracks();
        failMusic(preparationError);
        return false;
    }
    __atomic_store_n(&g_running, 1, __ATOMIC_RELEASE);
    int32_t rc = scePthreadCreate(&g_thread, 0, musicWorker, 0, "peppy-music");
    if (rc != 0) {
        closePreparedTracks();
        failMusic(rc);
        __atomic_store_n(&g_running, 0, __ATOMIC_RELEASE);
        return false;
    }
    g_joinable = true;
    return true;
}

void musicSetVolume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    __atomic_store_n(&g_volume, percent, __ATOMIC_RELEASE);
}
void musicToggleMute() { __atomic_fetch_xor(&g_muted, 1, __ATOMIC_ACQ_REL); }
void musicNextTrack() { __atomic_store_n(&g_skip, 1, __ATOMIC_RELEASE); }
void musicStop() { __atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE); }
void musicShutdown() {
    musicStop();
    if (g_joinable) {
        scePthreadJoin(g_thread, 0);
        g_joinable = false;
    }
}
MusicSnapshot musicSnapshot() {
    MusicSnapshot result;
    result.state = __atomic_load_n(&g_state, __ATOMIC_ACQUIRE);
    result.track = __atomic_load_n(&g_track, __ATOMIC_ACQUIRE);
    result.volume = __atomic_load_n(&g_volume, __ATOMIC_ACQUIRE);
    result.muted = __atomic_load_n(&g_muted, __ATOMIC_ACQUIRE) != 0;
    result.errorCode = __atomic_load_n(&g_error, __ATOMIC_ACQUIRE);
    return result;
}
const char* musicTrackName(int track) { return track == 1 ? "ACENDAOFAROL" : "FIGHT"; }
