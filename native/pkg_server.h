#pragma once

#include <stdint.h>
#include <stdio.h>

// A temporary 127.0.0.1-only HTTP source for one already validated package.
// The owner lends exclusive use of FILE until stop returns; this class never
// closes/reopens the FILE or resolves a package path. Control calls are serialized.
class PkgServer {
public:
    PkgServer();
    ~PkgServer();
    int32_t start(FILE* file, uint64_t bytes);
    int32_t errorCode() const;
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
