#pragma once

#include <stdint.h>
#include <stdio.h>

// Last attempted HTTP response and cumulative successfully sent package bytes.
// A request includes malformed/incomplete clients; headers do not count as bytes.
// Kept after stop(), and reset only when starting a new source.
struct PkgServerSnapshot {
    uint32_t requests;
    int32_t status;
    uint64_t sentBytes;
};

// A temporary 127.0.0.1-only HTTP source for one already validated package.
// The owner lends exclusive use of FILE until stop returns; this class never
// closes/reopens the FILE or resolves a package path. Control calls are serialized.
class PkgServer {
public:
    PkgServer();
    ~PkgServer();
    int32_t start(FILE* file, uint64_t bytes);
    int32_t errorCode() const;
    PkgServerSnapshot snapshot() const;
    const char* url() const;
    // On every return (including an error), the worker has finished using FILE.
    // Only this worker closes active sockets, avoiding close/reused-fd races.
    // Returns cleanup failures only; errorCode() retains startup/transfer errors.
    int32_t stop();
private:
    struct State;
    State* state_;
    static void* worker(void* argument);
    static void serve(State* state, int32_t client);
    PkgServer(const PkgServer&);
    PkgServer& operator=(const PkgServer&);
};
