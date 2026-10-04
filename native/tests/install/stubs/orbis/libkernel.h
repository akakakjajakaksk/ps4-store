#include "stub.h"

// Deliberately opaque: the native kernel writes its own 120-byte ABI, not
// Linux's or OpenOrbis musl's struct stat. The mock serializes native bytes.
extern "C" int32_t sceKernelFstat(int32_t descriptor, void* nativeInfo);
