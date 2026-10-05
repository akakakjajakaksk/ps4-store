#ifndef PEPPY_PARALLEL_DOWNLOAD_H
#define PEPPY_PARALLEL_DOWNLOAD_H

#include "downloads.h"
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <orbis/libkernel.h>

// The SDK declares these returns unsigned. Native libkernel and its public
// implementations return signed 64-bit counts or encoded kernel errors.
extern "C" int64_t peppyParallelPwrite(int32_t, const void*, size_t, int64_t)
    __asm__("sceKernelPwrite");
extern "C" int64_t peppyParallelPread(int32_t, void*, size_t, int64_t)
    __asm__("sceKernelPread");
// OpenOrbis's packaged POSIX stat is 128 bytes; libkernel writes this native
// 120-byte layout. Do not pass the incompatible libc type to sceKernelFstat.
struct PeppyParallelFileInfo {
    uint32_t device, inode;
    uint16_t mode, links;
    uint32_t uid, gid, rdev;
    int64_t accessTime[2], modificationTime[2], changeTime[2];
    int64_t size, blocks;
    int32_t blockSize;
    uint32_t flags, generation;
    int32_t spare;
    int64_t birthTime[2];
};
static_assert(sizeof(PeppyParallelFileInfo) == 120 && offsetof(PeppyParallelFileInfo, mode) == 8 &&
              offsetof(PeppyParallelFileInfo, size) == 72 && offsetof(PeppyParallelFileInfo, blocks) == 80,
              "PS4 native file metadata ABI");
extern "C" int32_t peppyParallelFstat(int32_t, PeppyParallelFileInfo*) __asm__("sceKernelFstat");

namespace peppyParallelDownload {
const size_t BUFFER_BYTES = 256 * 1024;
struct Failure {
    int category, where;
    int32_t native, network, ssl;
    uint32_t sslDetails;
    Failure() : category(0), where(0), native(0), network(0), ssl(0), sslDetails(0) {}
};
struct Lane {
    void* context;
    int32_t (*read)(void*, void*, size_t);
    void (*captureReadError)(void*, int32_t, Failure*);
    uint64_t first, length;
};
struct Plan {
    Lane lanes[2];
    int fd;
    void* context;
    bool (*cancelled)(void*);
    void (*abort)(void*);
    void (*progress)(void*, uint64_t);
};
struct Outcome {
    Failure failure;
    bool started;
    Outcome() : started(false) {}
};
namespace detail {
struct State {
    const Plan* plan;
    unsigned char* buffers[2];
    Failure failures[3];
    uint64_t done[2];
    int stop, winner, alive;
    explicit State(const Plan& p) : plan(&p), buffers{0,0}, done{0,0}, stop(0), winner(-1), alive(0) {}
    ~State() { free(buffers[1]); free(buffers[0]); }
};
inline bool stopped(State& s) {
    return __atomic_load_n(&s.stop, __ATOMIC_ACQUIRE) || s.plan->cancelled(s.plan->context);
}
inline void failure(State& s, int index, const Failure& value) {
    s.failures[index] = value;
    int expected = -1;
    if (__atomic_compare_exchange_n(&s.winner, &expected, index, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&s.stop, 1, __ATOMIC_RELEASE);
        s.plan->abort(s.plan->context);
    }
}
inline Failure simpleFailure(int category, int where, int32_t native) {
    Failure f; f.category = category; f.where = where; f.native = native; return f;
}
inline void runLane(State& s, int index) {
    const Lane& lane = s.plan->lanes[index];
    unsigned char* buffer = s.buffers[index];
    while (!stopped(s) && s.done[index] < lane.length) {
        uint64_t remaining = lane.length - s.done[index];
        size_t block = remaining < BUFFER_BYTES ? static_cast<size_t>(remaining) : BUFFER_BYTES;
        size_t filled = 0;
        // libSceHttp can return short fragments. Coalesce them before disk I/O
        // so parallel readers retain the sequential path's bounded buffering.
        while (filled < block && !stopped(s)) {
            size_t requested = block - filled;
            int32_t got = lane.read(lane.context, buffer + filled, requested);
            if (stopped(s)) break;
            if (got < 0) {
                Failure f; lane.captureReadError(lane.context, got, &f); failure(s, index, f); break;
            }
            if (got == 0 || static_cast<size_t>(got) > requested) {
                failure(s, index, simpleFailure(DOWNLOAD_ERROR_LENGTH, DOWNLOAD_STAGE_READ, got)); break;
            }
            filled += static_cast<size_t>(got);
        }
        if (stopped(s)) break;
        size_t written = 0;
        unsigned interruptions = 0;
        while (written < filled && !stopped(s)) {
            size_t required = filled - written;
            int64_t rc = peppyParallelPwrite(s.plan->fd, buffer + written, required,
                static_cast<int64_t>(lane.first + s.done[index]));
            // Native libkernel EINTR is encoded, not POSIX -1/errno.
            if (static_cast<uint32_t>(rc) == 0x80020004U && interruptions++ < 8) continue;
            if (rc <= 0 || static_cast<uint64_t>(rc) > required) {
                failure(s, index, simpleFailure(DOWNLOAD_ERROR_FILESYSTEM, DOWNLOAD_STAGE_FILE_WRITE,
                    rc ? static_cast<int32_t>(rc) : EIO)); break;
            }
            interruptions = 0;
            written += static_cast<size_t>(rc);
            s.done[index] += static_cast<uint64_t>(rc);
            s.plan->progress(s.plan->context, static_cast<uint64_t>(rc));
        }
    }
}
inline void* secondLane(void* value) {
    State& s = *static_cast<State*>(value);
    runLane(s, 1);
    __atomic_store_n(&s.alive, 0, __ATOMIC_RELEASE);
    return 0;
}
} // namespace detail

// The caller owns both validated HTTP responses and the descriptor. No caller
// resource can be closed, reused or truncated until this function returns.
inline Outcome run(const Plan& plan) {
    Outcome out;
    detail::State state(plan);
    state.buffers[0] = static_cast<unsigned char*>(malloc(BUFFER_BYTES));
    if (state.buffers[0]) state.buffers[1] = static_cast<unsigned char*>(malloc(BUFFER_BYTES));
    if (!state.buffers[0] || !state.buffers[1]) {
        out.failure = detail::simpleFailure(DOWNLOAD_ERROR_FILESYSTEM, DOWNLOAD_STAGE_FILE_OPEN, ENOMEM);
        return out;
    }
    OrbisPthreadAttr attr;
    int32_t rc = scePthreadAttrInit(&attr);
    bool attrReady = rc == 0;
    OrbisPthread thread;
    if (!rc) rc = scePthreadAttrSetdetachstate(&attr, 0); // Native JOINABLE.
    if (!rc) {
        __atomic_store_n(&state.alive, 1, __ATOMIC_RELEASE);
        rc = scePthreadCreate(&thread, &attr, detail::secondLane, &state, "peppy-range");
        if (rc) __atomic_store_n(&state.alive, 0, __ATOMIC_RELEASE);
    }
    if (attrReady) scePthreadAttrDestroy(&attr);
    if (rc) {
        out.failure = detail::simpleFailure(DOWNLOAD_ERROR_THREAD, DOWNLOAD_STAGE_THREAD, rc);
        return out;
    }
    out.started = true;
    detail::runLane(state, 0);
    if (plan.cancelled(plan.context) || __atomic_load_n(&state.stop, __ATOMIC_ACQUIRE))
        plan.abort(plan.context);
    int32_t joinRc = scePthreadJoin(thread, 0);
    if (joinRc) {
        detail::failure(state, 2, detail::simpleFailure(DOWNLOAD_ERROR_THREAD, DOWNLOAD_STAGE_THREAD, joinRc));
        // A failed native join is not permission to free a worker's buffers.
        // Abort wakes its request; wait until its last access has finished.
        while (__atomic_load_n(&state.alive, __ATOMIC_ACQUIRE)) sceKernelUsleep(1000);
        // Reap a handle after a transient join failure. The first native error
        // remains the result even if cleanup succeeds on this second call.
        scePthreadJoin(thread, 0);
    }
    int winner = __atomic_load_n(&state.winner, __ATOMIC_ACQUIRE);
    if (winner >= 0) out.failure = state.failures[winner];
    else if (!plan.cancelled(plan.context) &&
             (state.done[0] != plan.lanes[0].length || state.done[1] != plan.lanes[1].length))
        out.failure = detail::simpleFailure(DOWNLOAD_ERROR_LENGTH, DOWNLOAD_STAGE_READ, 0);
    return out;
}
} // namespace peppyParallelDownload
#endif
