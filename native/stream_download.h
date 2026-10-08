#ifndef PEPPY_STREAM_DOWNLOAD_H
#define PEPPY_STREAM_DOWNLOAD_H

#include "parallel_download.h"

// One HTTP response with a bounded producer/consumer queue. The receiver owns
// validation and each buffer until publication; the disk worker owns a buffer
// until its write/hash callback completes. No caller resource may be closed
// until run() has joined the worker (including after cancellation).
namespace peppyStreamDownload {
typedef peppyParallelDownload::Failure Failure;
struct Plan {
    peppyDownloadBuffers::Buffers* buffers;
    void* context;
    int32_t (*read)(void*, void*, size_t);
    void (*captureReadError)(void*, int32_t, Failure*);
    bool (*validate)(void*, const void*, size_t, Failure*);
    bool (*consume)(void*, const void*, size_t, Failure*);
    bool (*cancelled)(void*);
    void (*abort)(void*);
    void (*mode)(void*, bool);
    void (*progress)(void*, uint64_t);
    uint64_t length;
    bool framed;
};
struct Outcome {
    Failure failure;
    bool threaded;
    Outcome() : threaded(false) {}
};
namespace detail {
struct State {
    const Plan* plan;
    Failure failures[3];
    size_t counts[2];
    int ready[2], stop, winner, finished, alive;
    explicit State(const Plan& p) : plan(&p), counts{0,0}, ready{0,0},
        stop(0), winner(-1), finished(0), alive(0) {}
};
inline bool stopped(State& s) {
    return __atomic_load_n(&s.stop, __ATOMIC_ACQUIRE) || s.plan->cancelled(s.plan->context);
}
inline void fail(State& s, int index, const Failure& value) {
    s.failures[index] = value;
    int expected = -1;
    if (__atomic_compare_exchange_n(&s.winner, &expected, index, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&s.stop, 1, __ATOMIC_RELEASE);
        s.plan->abort(s.plan->context);
    }
}
inline Failure simpleFailure(int category, int where, int32_t native) {
    return peppyParallelDownload::detail::simpleFailure(category, where, native);
}
inline unsigned char* buffer(State& s, unsigned slot) {
    return slot ? s.plan->buffers->second : s.plan->buffers->first;
}
inline void* writer(void* value) {
    State& s = *static_cast<State*>(value);
    unsigned slot = 0;
    while (!stopped(s)) {
        if (!__atomic_load_n(&s.ready[slot], __ATOMIC_ACQUIRE)) {
            // finished is published after the last ready slot. Recheck after
            // acquiring it so a stale first ready read cannot drop that slot.
            if (__atomic_load_n(&s.finished, __ATOMIC_ACQUIRE) &&
                !__atomic_load_n(&s.ready[slot], __ATOMIC_ACQUIRE)) break;
            sceKernelUsleep(1000);
            continue;
        }
        Failure f;
        if (!s.plan->consume(s.plan->context, buffer(s, slot), s.counts[slot], &f)) {
            fail(s, 1, f);
            break;
        }
        __atomic_store_n(&s.ready[slot], 0, __ATOMIC_RELEASE);
        slot ^= 1;
    }
    __atomic_store_n(&s.alive, 0, __ATOMIC_RELEASE);
    return 0;
}
inline void receive(State& s, bool threaded) {
    const Plan& p = *s.plan;
    uint64_t received = 0;
    unsigned slot = 0;
    while (!stopped(s) && received < p.length) {
        while (threaded && __atomic_load_n(&s.ready[slot], __ATOMIC_ACQUIRE) && !stopped(s))
            sceKernelUsleep(1000);
        if (stopped(s)) break;
        const uint64_t remaining = p.length - received;
        const size_t block = remaining < p.buffers->capacity ? static_cast<size_t>(remaining) : p.buffers->capacity;
        unsigned char* bytes = buffer(s, slot);
        size_t filled = 0;
        while (filled < block && !stopped(s)) {
            const size_t requested = block - filled;
            const int32_t got = p.read(p.context, bytes + filled, requested);
            // A positive native read counts as received even when the user
            // cancels as that call finishes. It is never queued after cancel.
            if (got <= 0 && stopped(s)) break;
            Failure f;
            if (got < 0) { p.captureReadError(p.context, got, &f); fail(s, 0, f); break; }
            if (!got && !p.validate(p.context, bytes + filled, 0, &f)) {
                fail(s, 0, f); break;
            }
            if (!got || static_cast<size_t>(got) > requested) {
                fail(s, 0, simpleFailure(DOWNLOAD_ERROR_LENGTH, DOWNLOAD_STAGE_READ, got)); break;
            }
            if (!p.validate(p.context, bytes + filled, static_cast<size_t>(got), &f)) {
                fail(s, 0, f); break;
            }
            filled += static_cast<size_t>(got);
            received += static_cast<uint64_t>(got);
            p.progress(p.context, static_cast<uint64_t>(got));
        }
        if (stopped(s)) break;
        if (threaded) {
            s.counts[slot] = filled;
            __atomic_store_n(&s.ready[slot], 1, __ATOMIC_RELEASE);
            slot ^= 1;
        } else {
            Failure f;
            if (!p.consume(p.context, bytes, filled, &f)) { fail(s, 0, f); break; }
        }
    }
    // Verified HTTP Content-Length terminates without another native read.
    // Catalog length alone still needs EOF, with an extra byte kept off disk.
    if (!stopped(s) && received == p.length && !p.framed) {
        unsigned char excess;
        const int32_t got = p.read(p.context, &excess, 1);
        if (!stopped(s)) {
            Failure f;
            if (got < 0) { p.captureReadError(p.context, got, &f); fail(s, 0, f); }
            else if (got) fail(s, 0, simpleFailure(DOWNLOAD_ERROR_LENGTH, DOWNLOAD_STAGE_READ, got));
        }
    }
    __atomic_store_n(&s.finished, 1, __ATOMIC_RELEASE);
}
} // namespace detail

inline Outcome run(const Plan& plan) {
    Outcome out;
    detail::State state(plan);
    if (!plan.buffers || !plan.buffers->ready()) {
        out.failure = detail::simpleFailure(DOWNLOAD_ERROR_FILESYSTEM, DOWNLOAD_STAGE_FILE_OPEN, ENOMEM);
        return out;
    }
    OrbisPthreadAttr attr;
    int32_t rc = scePthreadAttrInit(&attr);
    const bool attrReady = rc == 0;
    OrbisPthread thread;
    if (!rc) rc = scePthreadAttrSetdetachstate(&attr, 0);
    if (!rc) {
        __atomic_store_n(&state.alive, 1, __ATOMIC_RELEASE);
        rc = scePthreadCreate(&thread, &attr, detail::writer, &state, "peppy-write");
        if (rc) __atomic_store_n(&state.alive, 0, __ATOMIC_RELEASE);
    }
    if (attrReady) scePthreadAttrDestroy(&attr);
    // Thread setup has not read or written response bytes. A synchronous
    // fallback uses the same buffer/validation/framing path without a restart.
    out.threaded = rc == 0;
    plan.mode(plan.context, out.threaded);
    detail::receive(state, out.threaded);
    if (out.threaded) {
        if (detail::stopped(state)) plan.abort(plan.context);
        const int32_t joinRc = scePthreadJoin(thread, 0);
        if (joinRc) {
            detail::fail(state, 2, detail::simpleFailure(DOWNLOAD_ERROR_THREAD, DOWNLOAD_STAGE_THREAD, joinRc));
            while (__atomic_load_n(&state.alive, __ATOMIC_ACQUIRE)) sceKernelUsleep(1000);
            scePthreadJoin(thread, 0);
        }
    }
    const int winner = __atomic_load_n(&state.winner, __ATOMIC_ACQUIRE);
    if (winner >= 0) out.failure = state.failures[winner];
    return out;
}
} // namespace peppyStreamDownload

#endif
