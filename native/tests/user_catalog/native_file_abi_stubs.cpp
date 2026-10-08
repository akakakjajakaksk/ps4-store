#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// Independent raw PS4 ABI fixture: no production struct/header is included.
// The musl wrappers deliberately fail to expose accidental native use of them.
extern "C" int __real_fstat(int descriptor, struct stat* info);
static bool failNative = false;
static unsigned nativeCalls = 0, forbiddenCalls = 0;
extern "C" void peppyTestCatalogFstatFailure(bool enabled) { failNative = enabled; }
extern "C" int __wrap_fstat(int, struct stat*) { ++forbiddenCalls; errno = ENOSYS; return -1; }
extern "C" int __wrap_lstat(const char*, struct stat*) { ++forbiddenCalls; errno = ENOSYS; return -1; }
template <typename T> void field(unsigned char* output, size_t offset, T value) {
    memcpy(output + offset, &value, sizeof(value));
}
extern "C" int32_t sceKernelFstat(int32_t descriptor, void* output) {
    ++nativeCalls;
    if (failNative) { errno = EINVAL; return (int32_t)0x8002004E; }
    struct stat host;
    if (__real_fstat(descriptor, &host) != 0) return (int32_t)(0x80020000U | (uint32_t)errno);
    unsigned char* bytes = (unsigned char*)output;
    memset(bytes, 0, 120);
    field<uint32_t>(bytes, 0, (uint32_t)host.st_dev);
    field<uint32_t>(bytes, 4, (uint32_t)host.st_ino);
    field<uint16_t>(bytes, 8, (uint16_t)host.st_mode);
    field<uint16_t>(bytes, 10, (uint16_t)host.st_nlink);
    field<uint32_t>(bytes, 12, (uint32_t)host.st_uid);
    field<uint32_t>(bytes, 16, (uint32_t)host.st_gid);
    field<uint32_t>(bytes, 20, (uint32_t)host.st_rdev);
    field<int64_t>(bytes, 24, host.st_atim.tv_sec);
    field<int64_t>(bytes, 32, host.st_atim.tv_nsec);
    field<int64_t>(bytes, 40, host.st_mtim.tv_sec);
    field<int64_t>(bytes, 48, host.st_mtim.tv_nsec);
    field<int64_t>(bytes, 56, host.st_ctim.tv_sec);
    field<int64_t>(bytes, 64, host.st_ctim.tv_nsec);
    field<int64_t>(bytes, 72, host.st_size);
    field<int64_t>(bytes, 80, host.st_blocks); // Deliberately different from size.
    field<int32_t>(bytes, 88, (int32_t)host.st_blksize);
    field<uint32_t>(bytes, 92, 0xAABBCCDDU);
    field<uint32_t>(bytes, 96, 0x11223344U);
    field<int32_t>(bytes, 100, 12345);
    field<int64_t>(bytes, 104, 987654321);
    field<int64_t>(bytes, 112, 123456789);
    return 0;
}
static void checkNativeCalls() {
    if (!nativeCalls || forbiddenCalls) {
        fprintf(stderr, "native metadata ABI: native calls %u, forbidden POSIX calls %u\n", nativeCalls, forbiddenCalls);
        abort();
    }
    printf("native metadata ABI: %u kernel-layout checks, no POSIX stat calls\n", nativeCalls);
}
struct AtExit { AtExit() { atexit(checkNativeCalls); } } atExit;
