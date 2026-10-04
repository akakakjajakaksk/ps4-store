#pragma once

#include <stdint.h>
#include <stdio.h>

// The installer tests replace only transport. The real local server has its
// own socket/HTTP suite; this mock verifies the borrowed FILE lifetime and
// the installer's ordering around BGFT ownership and cancellation.
struct PkgServerSnapshot {
    uint32_t requests;
    int32_t status;
    uint64_t sentBytes;
};

class PkgServer {
public:
    PkgServer();
    ~PkgServer();
    int32_t start(FILE* file, uint64_t size);
    const char* url() const;
    int32_t errorCode() const;
    PkgServerSnapshot snapshot() const;
    int32_t stop();
private:
    FILE* file_;
    int descriptor_;
    bool active_;
};
