This suite mocks only native PS4/GoldHEN calls. It never installs an application.

Run from `native/`:

```sh
g++ -std=c++11 -O2 -Wall -Wextra -Werror -pthread \
  -Itests/install/stubs tests/install/install_test.cpp \
  -Wl,--wrap=write -Wl,--wrap=fsync -Wl,--wrap=close -Wl,--wrap=fclose -Wl,--wrap=rename \
  -Wl,--wrap=lstat -Wl,--wrap=open -Wl,--wrap=__open_2 -Wl,--wrap=fstat \
  -o /tmp/peppy-install-tests
/tmp/peppy-install-tests
```

Coverage includes the native 64-bit BGFT ABI; GoldHEN version/jailbreak/restore;
retaining the validated descriptor across namespace changes; inode-verified
system paths and bounded fallback copy; partial writes, ENOSPC, fsync/close/
rename errors and partial cleanup; existing-app preservation and self-install
rejection; exact native module/service/task errors; opaque progress bits; no
success until BGFT completion and installed availability; cancellation and
native cleanup; unsafe-context retry rejection; repeated clean installations;
detached-thread startup failures; request generations; and failed registration
outputs that refer to an unowned existing task. Console behavior remains unverified.

The HTTP fallback cases replace `PkgServer` with a transport mock via
`PEPPY_PKG_SERVER_HEADER`. They cover unavailable SDK probes with errno 78, 1,
or 0, an unknown SDK version, and unchanged credentials with no global-path
copy. The suite checks the foreground user and native HTTP registration
parameters; preserves AppInstUtil/BGFT/registration errors; rejects a changed
source inode and invalid server URLs; observes startup, transfer and shutdown
errors; and requires native completion plus installed-app confirmation before
success. A failed registration may return another task's ID without permitting
stop/unregister calls against it. Cancellation checks the order of owned task
stop, unregister, quiescent server shutdown and source `fclose`, including
cleanup failures. The source remains readable until server shutdown even when
shutdown reports an error. The real server's socket/HTTP behavior is tested
separately in its own suite.

HTTP registration uses the native `sceBgftServiceIntDownloadRegisterTask`
export with a 104-byte parameter structure and 64-bit package size at offset
96. The checked foreground-user query returns a signed native status.
[ezRemote's loopback installer](https://github.com/cy33hc/ps4-ezremote-client/blob/master/source/installer.cpp#L933)
provides precedent for a direct PKG URL; a JSON manifest is not required for
that flow. SDK version `0x100` retains scoped GoldHEN elevation and storage
registration. An unavailable or unknown SDK selects HTTP with unchanged
credentials; a failed jailbreak attempt remains a hard failure. This path
adds no firmware-specific kernel writes or assumed credential offsets.

All installer cases run with `lstat` forced to `ENOSYS`, matching
[OpenOrbis musl's PS4 fstatat stub](https://github.com/OpenOrbis/musl/blob/master/src/stat/fstatat.c).
The installer instead opens with `O_NOFOLLOW` and checks the same descriptor
using the native `sceKernelFstat` export and an explicit 120-byte kernel layout.
The packaged PS4 libc has a 32-bit `mode_t`, making its POSIX `struct stat`
128 bytes with `st_size` at offset 80. The native kernel uses a 16-bit mode
and puts size at offset 72, as defined in
[OpenOrbis' native kernel types](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/v0.5.4/include/orbis/_types/kernel.h).
The conflicting mode width comes from
[musl's PS4 architecture types](https://github.com/OpenOrbis/musl/blob/master/arch/ps4/bits/alltypes.h.in).
Passing the libc structure would read the block
count as file size. The mock writes exactly 120 bytes by independent offsets;
a guarded alias probe checks all key fields and a sparse file larger than 4 GiB.
The suite also forces POSIX `fstat` to `ENOSYS` and verifies it is never called.
Injected native failures preserve the raw `0x8002004E` code even with stale errno.
[PS4 flags](https://github.com/OpenOrbis/musl/blob/master/arch/ps4/bits/fcntl.h)
are checked at native compile time, along with the explicit native stat ABI. Tests also
refuse source/directory symlinks and FIFOs, preserve fstat failures, safely copy
different or symlinked global aliases, and reject a replaced partial-copy inode.
