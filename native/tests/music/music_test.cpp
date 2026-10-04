#include <assert.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

#define PEPPY_MUSIC_DIRECTORY "/tmp/peppy-music-tests"
#include "../../music.cpp"

namespace {
std::atomic<int> initResult(0), moduleResult(0), openResult(7), createResult(0), outputFailAfter(-1);
std::atomic<bool> moduleLoaded(true);
std::atomic<bool> holdOutput(false), waitingOutput(false);
std::atomic<int> closeCount(0), openCount(0), outputs(0), loadCount(0);
const int16_t* outstanding = 0;
std::mutex observationLock;
std::vector<int16_t> observed;

void put16(std::vector<unsigned char>& bytes, uint16_t value) {
    bytes.push_back(value & 255); bytes.push_back(value >> 8);
}
void put32(std::vector<unsigned char>& bytes, uint32_t value) {
    for (int i = 0; i < 4; ++i) bytes.push_back((value >> (8 * i)) & 255);
}
void putTag(std::vector<unsigned char>& bytes, const char* tag) {
    bytes.insert(bytes.end(), tag, tag + 4);
}
std::vector<unsigned char> waveBytes(int16_t sample, unsigned frames = 258, bool junk = false) {
    std::vector<unsigned char> bytes;
    putTag(bytes, "RIFF"); put32(bytes, 0); putTag(bytes, "WAVE");
    if (junk) { putTag(bytes, "JUNK"); put32(bytes, 3); putTag(bytes, "abc "); }
    putTag(bytes, "fmt "); put32(bytes, 16); put16(bytes, 1); put16(bytes, 2);
    put32(bytes, 48000); put32(bytes, 192000); put16(bytes, 4); put16(bytes, 16);
    putTag(bytes, "data"); put32(bytes, frames * 4);
    for (unsigned i = 0; i < frames * 2; ++i) put16(bytes, uint16_t(sample));
    uint32_t riff = bytes.size() - 8;
    for (int i = 0; i < 4; ++i) bytes[4 + i] = (riff >> (8 * i)) & 255;
    return bytes;
}
void writeFixture(const char* name, const std::vector<unsigned char>& bytes) {
    std::ofstream stream(std::string(PEPPY_MUSIC_DIRECTORY) + "/" + name, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
template<class Predicate> void await(Predicate ready) {
    for (int i = 0; i < 2000; ++i) {
        if (ready()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(!"Timed out waiting for music worker");
}
void reset() {
    holdOutput = false;
    musicShutdown();
    initResult = 0; moduleResult = 0; moduleLoaded = true; openResult = 7;
    outputFailAfter = -1; createResult = 0; closeCount = 0; openCount = 0; outputs = 0; loadCount = 0;
    outstanding = 0;
    waitingOutput = false;
    musicSetVolume(30);
    if (musicSnapshot().muted) musicToggleMute();
    std::lock_guard<std::mutex> lock(observationLock);
    observed.clear();
}
bool saw(int16_t value) {
    std::lock_guard<std::mutex> lock(observationLock);
    for (size_t i = 0; i < observed.size(); ++i) if (observed[i] == value) return true;
    return false;
}
} // namespace

extern "C" int32_t sceAudioOutInit() { return initResult; }
extern "C" int32_t sceAudioOutOpen(int32_t user, OrbisAudioOutPort port, int32_t index,
    uint32_t frames, uint32_t frequency, uint32_t format) {
    assert(user >= 0 && port == ORBIS_AUDIO_OUT_PORT_TYPE_MAIN && index == 0);
    assert(frames == FRAMES && frequency == 48000 && format == ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    ++openCount;
    return openResult;
}
extern "C" int32_t sceAudioOutOutput(int32_t port, const void* samples) {
    assert(port == 7);
    if (!samples) {
        waitingOutput = true;
        while (holdOutput) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        waitingOutput = false;
        if (outstanding) {
            std::lock_guard<std::mutex> lock(observationLock);
            observed.push_back(outstanding[0]);
            outstanding = 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return 0;
    }
    assert(!outstanding); // Every reused buffer must first finish its prior output.
    if (outputFailAfter >= 0 && outputs >= outputFailAfter) return int32_t(0x80260001u);
    ++outputs;
    outstanding = static_cast<const int16_t*>(samples);
    return 0;
}
extern "C" int32_t sceAudioOutClose(int32_t) { assert(!outstanding); ++closeCount; return 0; }
extern "C" int32_t sceSysmoduleIsLoadedInternal(OrbisSysModuleInternal) { return moduleLoaded ? 0 : -1; }
extern "C" uint32_t sceSysmoduleLoadModuleInternal(OrbisSysModuleInternal) {
    ++loadCount;
    if (!moduleResult) moduleLoaded = true;
    return uint32_t(moduleResult.load());
}
extern "C" int32_t scePthreadCreate(OrbisPthread* thread, const OrbisPthreadAttr* attr,
    void* (*worker)(void*), void* argument, const char*) {
    if (createResult) return createResult;
    return pthread_create(thread, attr, worker, argument);
}
extern "C" int32_t scePthreadJoin(OrbisPthread thread, void** result) { return pthread_join(thread, result); }

int main() {
    mkdir(PEPPY_MUSIC_DIRECTORY, 0700);
    writeFixture("fight.wav", waveBytes(10000, 258, true));
    writeFixture("acendaofarol.wav", waveBytes(20000));
    WavStream wav;
    assert(openWav(TRACK_FILES[0], wav) == 0 && wav.remaining == 258 * 4);
    int16_t buffer[FRAMES * CHANNELS];
    assert(readFrames(wav, buffer) == 0 && buffer[0] == 10000 && wav.remaining == 8);
    assert(readFrames(wav, buffer) == 0 && buffer[3] == 10000 && buffer[4] == 0);
    closeWav(wav);

    std::vector<unsigned char> malformed = waveBytes(10000);
    malformed[22] = 1; // Mono is not the packaged format.
    writeFixture("invalid.wav", malformed);
    assert(openWav(PEPPY_MUSIC_DIRECTORY "/invalid.wav", wav) == MUSIC_ERROR_FORMAT);
    malformed = waveBytes(10000); malformed.resize(malformed.size() - 1);
    writeFixture("invalid.wav", malformed);
    assert(openWav(PEPPY_MUSIC_DIRECTORY "/invalid.wav", wav) == MUSIC_ERROR_FORMAT);
    malformed = waveBytes(10000); malformed[40] = 1; // Misaligned data frames.
    writeFixture("invalid.wav", malformed);
    assert(openWav(PEPPY_MUSIC_DIRECTORY "/invalid.wav", wav) == MUSIC_ERROR_FORMAT);
    malformed = waveBytes(10000, 258, true);
    memset(malformed.data() + 16, 255, 4); // Oversized JUNK cannot allocate or skip past RIFF.
    writeFixture("invalid.wav", malformed);
    assert(openWav(PEPPY_MUSIC_DIRECTORY "/invalid.wav", wav) == MUSIC_ERROR_FORMAT);

    writeFixture("fight.wav", waveBytes(10000, 8192));
    writeFixture("acendaofarol.wav", waveBytes(20000, 8192));
    reset();
    assert(musicStart(42) && !musicStart(42));
    // Both descriptors are held before musicStart returns. Simulate /app0 paths
    // disappearing while installation changes the process root.
    assert(g_prepared[0].file && g_prepared[1].file);
    assert(unlink(TRACK_FILES[0]) == 0 && unlink(TRACK_FILES[1]) == 0);
    await([] { return saw(3000) && saw(6000) && outputs >= 6; });
    assert(musicSnapshot().state == MUSIC_PLAYING && openCount == 1 && loadCount == 0);
    musicToggleMute();
    await([] { return saw(0); });
    assert(musicSnapshot().muted);
    musicToggleMute(); musicSetVolume(100);
    await([] { return saw(10000) && saw(20000); });
    holdOutput = true;
    await([] { return waitingOutput.load(); });
    int beforeSkip = musicSnapshot().track;
    musicNextTrack();
    holdOutput = false;
    await([beforeSkip] { return musicSnapshot().track == (beforeSkip + 1) % 2; });
    musicStop(); musicShutdown();
    assert(musicSnapshot().state == MUSIC_STOPPED && closeCount == 1);
    assert(!g_prepared[0].file && !g_prepared[1].file);
    writeFixture("fight.wav", waveBytes(10000));
    writeFixture("acendaofarol.wav", waveBytes(20000));
    musicSetVolume(150); assert(musicSnapshot().volume == 100);
    musicSetVolume(-10); assert(musicSnapshot().volume == 0);

    reset(); initResult = int32_t(ORBIS_AUDIO_OUT_ERROR_ALREADY_INIT);
    assert(musicStart(-1)); await([] { return musicSnapshot().state == MUSIC_PLAYING; });
    musicShutdown(); assert(closeCount == 1);

    reset(); moduleLoaded = false; moduleResult = -123;
    assert(musicStart(42)); await([] { return musicSnapshot().state == MUSIC_FAILED; });
    musicShutdown(); assert(musicSnapshot().errorCode == -123 && openCount == 0 && closeCount == 0);

    reset(); moduleLoaded = false;
    assert(musicStart(42)); await([] { return musicSnapshot().state == MUSIC_PLAYING; });
    musicShutdown(); assert(loadCount == 1 && closeCount == 1);

    reset(); initResult = -124;
    assert(musicStart(42)); await([] { return musicSnapshot().state == MUSIC_FAILED; });
    musicShutdown(); assert(musicSnapshot().errorCode == -124 && openCount == 0);

    reset(); openResult = -125;
    assert(musicStart(42)); await([] { return musicSnapshot().state == MUSIC_FAILED; });
    musicShutdown(); assert(musicSnapshot().errorCode == -125 && closeCount == 0);

    reset(); outputFailAfter = 1;
    assert(musicStart(42)); await([] { return musicSnapshot().state == MUSIC_FAILED; });
    musicShutdown(); assert(musicSnapshot().errorCode == int32_t(0x80260001u) && closeCount == 1);

    reset(); createResult = -126;
    assert(!musicStart(42) && musicSnapshot().state == MUSIC_FAILED && musicSnapshot().errorCode == -126);
    assert(!g_prepared[0].file && !g_prepared[1].file);
    reset();
    unlink(TRACK_FILES[0]);
    assert(musicStart(42)); await([] { return saw(6000); });
    musicShutdown(); assert(closeCount == 1); // A missing first song does not suppress the second.
    reset(); unlink(TRACK_FILES[1]);
    assert(!musicStart(42) && musicSnapshot().state == MUSIC_FAILED);
    musicShutdown(); assert(musicSnapshot().errorCode == MUSIC_ERROR_FILE && closeCount == 0);
    assert(!g_prepared[0].file && !g_prepared[1].file);
    unlink(PEPPY_MUSIC_DIRECTORY "/invalid.wav"); rmdir(PEPPY_MUSIC_DIRECTORY);
    puts("Music tests passed: bounded PCM streaming, playlist, gain/mute, lifecycle and native failures.");
}
