This suite mocks only native PS4/GoldHEN calls. It never installs an application.

Run from `native/`:

```sh
g++ -std=c++11 -O2 -Wall -Wextra -Werror -pthread \
  -Itests/install/stubs tests/install/install_test.cpp \
  -Wl,--wrap=write -Wl,--wrap=fsync -Wl,--wrap=close -Wl,--wrap=rename \
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

All installer cases run with `lstat` forced to `ENOSYS`, matching
[OpenOrbis musl's PS4 fstatat stub](https://github.com/OpenOrbis/musl/blob/master/src/stat/fstatat.c).
The installer instead opens with `O_NOFOLLOW` and checks the same descriptor
using [PS4's dedicated fstat wrapper](https://github.com/OpenOrbis/musl/blob/master/src/stat/fstat.c).
[PS4 flags](https://github.com/OpenOrbis/musl/blob/master/arch/ps4/bits/fcntl.h)
are checked at native compile time, along with the 120-byte stat ABI. Tests also
refuse source/directory symlinks and FIFOs, preserve fstat failures, safely copy
different or symlinked global aliases, and reject a replaced partial-copy inode.
