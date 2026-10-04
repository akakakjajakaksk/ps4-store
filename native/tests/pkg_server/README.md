# Loopback PKG source checks

From the repository root:

```sh
g++ -std=c++11 -O2 -Wall -Wextra -Werror -pthread \
  -Inative/tests/pkg_server/stubs native/tests/pkg_server/server_test.cpp \
  -o /tmp/peppy-pkg-server-tests-bin
/tmp/peppy-pkg-server-tests-bin
```

SceNet mocks do not contact the network. They verify the PS4 16-byte sockaddr
layout, binding only 127.0.0.1 with an ephemeral port, native SOL_SOCKET/SO_NBIO
constants, fragmented headers, partial sends, EINTR/EWOULDBLOCK, cancellation,
single-close ownership and preservation of the borrowed FILE. Protocol checks
cover GET/HEAD, exact paths, malformed requests, a finite 8 KiB header cap,
duplicate/multiple ranges, suffix/open/closed ranges, overflow and 416 responses.
A sparse-file range tests actual file offsets beyond 4 GiB.

The owner must keep its validated FILE open and exclusively borrowed until
`stop()` returns. The server does not reopen paths or close that FILE. Its one
worker bounds each read/output block to 64 KiB and closes its own sockets before
publishing completion. Shutdown joins the worker, destroys only its own network
pool and does not terminate/unload shared networking. Native blocking-socket
configuration failures stop startup; no blocking fallback is used.

`stop()` returns only a cleanup failure. An ordinary startup/transfer failure
remains available through `errorCode()` after a clean stop, allowing the owner
to retain the PKG and retry without treating delivery errors as failed cleanup.
OpenOrbis 0.5.4 declares `sceNetPoolDestroy` as void, which is used directly.
HEAD ignores Range and returns the full representation's headers, per RFC 7233.

Socket definitions are from [OpenOrbis v0.5.4](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/v0.5.4/include/orbis/Net.h).
The missing PS4 socket option/error constants match public
[shadPS4 network/net.h](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/network/net.h)
and [network/net_error.h](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/network/net_error.h).
Native PS4/BGFT use still needs console validation.
