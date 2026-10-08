The native hub worker is tested with the existing OpenOrbis HTTP/thread stubs,
mock HTTP replies and the real user-catalog parser. It does not access a network
or write login credentials to files.

```sh
g++ -std=c++11 -O2 -Wall -Wextra -Werror -pthread \
  -Inative/tests/downloads/stubs \
  native/tests/hub_client/hub_test.cpp native/user_catalog.cpp \
  -o /tmp/peppy-hub-test
/tmp/peppy-hub-test
```

The checks cover asynchronous login/logout, server session revalidation before
catalog synchronization, local session/premium expiry, administrator role checks,
publication requests, failed thread initialization/creation, malformed catalog
metadata, unsafe origins, private DNS replies, cancellation, exact bounded HTTP
framing, unsupported range responses, provider-scoped redirects and withholding
the hub bearer token from PKG hosts. Real package-header imports are exercised
through the same range callback used on the console. Header/SFO parsing and
catalog persistence have their own tests under `tests/user_catalog`.

The hub origin must be an explicitly trusted HTTPS service; changing it clears
the current session. The origin contains no password. Credentials and bearer
tokens remain in RAM and are cleared after use/logout/expiry. Login/session
`server_time` and `expires_at` establish monotonic RAM deadlines, so changing the
console calendar cannot extend a session. Session TTL is capped at 24 hours;
catalog synchronization rechecks the session with the server. API replies require
identity encoding and a matching Content-Length no larger than 4 MiB. A failed
catalog parse preserves the UI's previous catalog because results transfer only
after the complete worker operation finishes.

Native PKG metadata requests require exactly framed HTTP 206 byte ranges. A
server that ignores Range is rejected before its full file is read. Imports reuse
one HTTP context and transfer less than 100 KB per package. Source URLs must
match the same reviewed providers and Archive collection/path policy as the full
downloader. Package redirects stay within their provider. Metadata cannot prove
licensing, a publisher signature, or compatibility with firmware 13.52; full
downloads still pin their actual header kind, Content ID and declared size.

OpenOrbis libSceHttp exposes no supported API to pin a DNS answer while retaining
the original HTTPS hostname. A public IPv4 DNS preflight therefore supplements
normal certificate/hostname validation, and arbitrary user-controlled package
hosts remain unsupported. This is not a claim of general-purpose SSRF-safe
fetching: pages, shorteners, arbitrary domains and unsupported sources return a
clear direct-PKG/source error. Hub credentials go only to the configured origin;
hub requests never follow redirects.
