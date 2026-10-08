The native hub worker is tested with the existing OpenOrbis HTTP/thread stubs,
mock HTTP replies and the real user-catalog parser. It does not access a network.
Saved-login checks use disposable private directories and synthetic test-only
credentials; they never read or write a real owner's account.

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
the current session and prevents reuse of another origin's remembered record.
The origin contains no password. If `configureHubSavedLogin()` is enabled, the
last successful username, password and revocable bearer are saved only under
`/data/peppy-store/private/login.dat`. The child directory is 0700 and the file
is 0600, independent of older builds' 0777 outer FTP/app directory. The record
contains no role or premium access flag. This is filesystem privacy, not a
platform keychain or encryption; credentials are never included in the PKG,
source code, hub catalog, logs or FTP inbox.

Startup restoration runs asynchronously and validates the bearer with the
trusted server before granting any access. If a denied saved token's original
server expiry has already passed according to the console calendar, restoration
can try the remembered password once through the ordinary login endpoint. It
cannot turn a revoked account into a valid one. A non-expired token denial, a
heartbeat denial and explicit logout erase the remembered password/bearer while
retaining the username. Heartbeats never automatically relogin. An offline
verification failure still closes RAM authorization after 30 seconds but keeps
the local record for a later startup or user retry. Wrong entered credentials,
cancelled restores and transient startup network failures do not replace the
last successful saved login. Logging out also forgets it when no RAM session
remains. Atomic writes use a private temporary file, fsync and rename; bounded
reads reject symlinks, hardlinks, unsafe permissions and corrupt records using
the actual 120-byte PS4 `sceKernelFstat` ABI.

Login/session
`server_time` and `expires_at` establish monotonic RAM deadlines, so changing the
console calendar cannot extend authorized access. Native session TTL is capped
at 24 hours and the current service issues eight-hour bearer sessions;
catalog synchronization rechecks the session with the server. API replies require
identity encoding and a matching Content-Length no larger than 4 MiB. A failed
catalog parse preserves the UI's previous catalog because results transfer only
after the complete worker operation finishes.

Native PKG metadata requests require exactly framed HTTP 206 byte ranges. A
server that ignores Range is rejected before its full file is read. Imports reuse
one HTTP context and transfer less than 100 KB of PKG metadata per package. Source URLs must
match the same reviewed providers and Archive collection/path policy as the full
downloader. Package redirects stay within their provider. Metadata cannot prove
licensing, a publisher signature, or compatibility with firmware 13.52; full
downloads still pin their actual header kind, Content ID and declared size.

OpenOrbis libSceHttp exposes no supported API to pin a DNS answer while retaining
the original HTTPS hostname. A public IPv4 DNS preflight therefore supplements
normal certificate/hostname validation, and arbitrary user-controlled package
hosts remain unsupported. This is not a claim of general-purpose SSRF-safe
fetching: unsupported pages, shorteners, arbitrary domains and sources return a
clear direct-PKG/source error. Hub credentials go only to the configured origin;
hub requests never follow redirects.

Account management lists at most 5,000 bounded metadata records, including the
bootstrap administrator's special `owner` ID. Only premium targets with a
canonical 32-character ID can be invalidated/reactivated or have their password
changed. Native requests use authenticated POST `/api/admin/users/:id/revocation`
and `/api/admin/users/:id/password` aliases; the server retains its PATCH API.
The UI copies the selected account identity before editing/confirmation, so a
refreshed or reordered account list cannot switch the target. Password editors
are masked and their RAM copies are cleared after transfer to the worker.

A separate session worker polls GET `/api/session` every five seconds, including
during imports, downloads, credential editing and system installation. It reads
no catalog and caps the response at 8 KiB. Each worker owns its HTTP context;
heartbeat failures do not overwrite the primary worker's progress, abort handle
or pending result. Generation checks reject delayed success/401 replies for a
logged-out or replaced session. Authorization clears on server denial, or after
30 seconds without successful server verification; transport failures do not
extend this grace. This is bounded polling and requires a connection, not a
claim of zero-latency propagation.

Regression checks run a held import request and a revocation heartbeat in actual
concurrent host threads, then cancel the original import using its retained
request handle. They also exercise stale checks across replacement login,
network grace expiry, administrator-only invalidation/password rotation and
malformed/duplicate user records. Exact-renderer controller checks cover account
selection/reordering, premium download cancellation, blocked automatic install
handoff after revocation, and retaining metadata through an existing PS4 system
installation while free downloads continue. They do not simulate a physical PS4.

Stable MediaFire `/file/<key>/<filename.pkg>/file` URLs are the narrow supported
landing-page exception. An anonymous HTTPS GET reads at most 1 MiB of identity
HTML and the shared pure parser accepts one actual `downloadButton` anchor on
a supported MediaFire CDN. No JavaScript, authentication, captcha handling or
additional providers are used. Known Content-Length ends without a trailing EOF
read; unknown/dechunked bodies require EOF and an extra byte rejects overflow
even at the exact cap. Provider redirects share the existing five-hop budget;
a CDN cannot return to a landing page or redirect to another provider. Exact
206 range/size checks still apply to the final PKG. The import context reuses
that CDN across its subsequent metadata reads, while entries retain the stable
page URL so normal downloads resolve a fresh public URL when needed. Fixtures
cover fragmented/known/unknown/exact-cap HTML, overflow/truncation, malformed
anchors, challenge pages, unsafe redirects and withholding hub credentials.
