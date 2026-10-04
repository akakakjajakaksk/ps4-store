# Downloader host tests

Run from `native/` on Linux with GNU g++ and GNU ld:

```sh
g++ -std=c++11 -O2 -Wall -Wextra -Werror -pthread \
  -Itests/downloads/stubs tests/downloads/downloads_test.cpp \
  -Wl,--wrap=fwrite -Wl,--wrap=fflush -Wl,--wrap=fclose \
  -Wl,--wrap=malloc -Wl,--wrap=setvbuf \
  -o /tmp/peppy-download-tests
/tmp/peppy-download-tests
```

The URL and HTML parser also has a standalone suite without native mocks:

```sh
g++ -std=c++11 -O2 -Wall -Wextra -Werror \
  tests/downloads/mediafire_parser_test.cpp -o /tmp/peppy-mediafire-parser-tests
/tmp/peppy-mediafire-parser-tests
```

The shim preserves the OpenOrbis 0.5.4 declarations needed by the downloader, including incomplete SDK declarations handled through asm aliases. It replaces network and thread calls with deterministic host mocks and uses a unique temporary directory per run. It does not contact GitHub, MediaFire or a PS4. All source pages, CDN paths and tokens are synthetic fixtures.

Coverage includes four SHA-256 NIST vectors, fragmented PKG magic, normal and redirected transfers, strict HTTPS/host/filename checks, redirect limits, HTTP/TLS errors, truncation and overflow, missing content lengths, mismatched digests, write/flush/close failures, preservation of an existing complete file on a failed replacement, detached worker creation, one transfer at a time, cancellation, and handle/partial-file cleanup. Initialization checks cover already-loaded modules, a failed load with a confirmed loaded probe, failed module loading, NetCtl initialization/state errors, bounded local-IP readiness, cancellation during readiness, pool errors, individual timeout errors, original native/SSL diagnostics, and repeated download cleanup. Error fixture numbers are mock values, not a whitelist of hardware errors.

The transfer writes a numeric diagnostic log at `/data/peppy-store/downloads/download.log` on a PS4 (inside the unique test directory on the host). Tests check that the log contains no URLs or signed tokens.

The console regression `0x80431073` (`TOO_LARGE_RESPONSE_HEADER`) is modeled with the native 5,000-byte default: an 8 KiB response fails with that default, then a redirected download succeeds after the template sets 64 KiB and the connection inherits it. Additional checks reject responses above 64 KiB, retain the original error and dedicated category, validate parser boundaries, and release all handles when configuring the limit fails.

Large-package cases advertise native 64-bit Content-Length values of 5 GiB
plus 123 bytes and exactly 256 GiB. They transfer only an eight-byte PKG
fixture before an injected read error, verifying acceptance, full-width total
progress, preservation of a prior complete file and partial-file cleanup
without allocating or streaming gigabytes. Exactly 256 GiB is also accepted
without catalog length metadata; oversized responses, specifications above
the common 256 GiB limit and UINT64_MAX are rejected at their proper stages.
Production download and installation bounds share `package_limits.h` and
require 64-bit file offsets and native response lengths.

MediaFire integration cases distinguish the original `/file/<id>/<name>/file`
page from the resolved `download<digits>.mediafire.com` HTTPS request. They
read both HTML and PKG in three-byte fragments, decode `&amp;` in the quoted
download anchor and verify that HTML neither contributes package progress nor
opens a partial output file. Missing links, CAPTCHA pages, script/comment
lookalikes, ambiguous anchors, invalid hosts, ports and provider changes fail
while preserving a previous complete package. Known and unknown HTML lengths
cover exactly 1 MiB, oversized declarations, a one-byte body excess and both
truncated and underestimated declared lengths.

The source mocks retain native SEND, STATUS, content-length and read errors,
including SSL and network diagnostics. A blocked HTML read exercises native
abort and cleanup on cancellation. Redirect cases cover allowed page and CDN
transitions, prohibited provider changes, loop termination and the common
five-transition budget, including page resolution. A second download fetches
the source page again and uses a fresh CDN token. PKG magic, final response
length and SHA-256 checks still run after source resolution. These cases
validate mocked control flow; they do not prove that a current MediaFire page
or CDN transfer works on a console.

Content ID cases pass the optional second argument to `startDownload` while
retaining the original four-field `DownloadSpec` and one-argument callers.
Synthetic PKG fixtures contain the first `0x438` bytes, with the 36-character
ID at `0x40`, big-endian content type `0x1A` at `0x74`, exact application flags
`0x0A000000` or `0x0E000000` at `0x78`, and big-endian 64-bit package size at
`0x430`. Both GitHub and MediaFire transfers fragment these headers into
three-byte reads. Matching identities and both accepted flags complete;
wrong identities, content types, patch/unknown flag values, wrong declared
sizes and truncated headers fail before replacing an existing file.

Specification cases reject malformed IDs and expected sizes below `0x438`,
including unknown size zero, when an identity is supplied. Null or empty IDs
retain the existing eight-byte homebrew fixtures. Tests also cover copying
the caller's ID before asynchronous work and a matching 5 GiB + 123-byte
header declaration followed by an injected native read failure after only
`0x438` fixture bytes. This checks full-width size handling without streaming
gigabytes. The header checks establish the declared identity and application
fields; they do not authenticate package contents or prove installability.

Additional host checks use the same linker wrappers:

```sh
g++ -std=c++11 -O2 -Wall -Wextra -Werror -D_FORTIFY_SOURCE=3 -pthread \
  -Itests/downloads/stubs tests/downloads/downloads_test.cpp \
  -Wl,--wrap=fwrite -Wl,--wrap=fflush -Wl,--wrap=fclose \
  -Wl,--wrap=malloc -Wl,--wrap=setvbuf \
  -o /tmp/peppy-download-tests-fortify
/tmp/peppy-download-tests-fortify
g++ -std=c++11 -O1 -g -Wall -Wextra -Werror -pthread \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Itests/downloads/stubs tests/downloads/downloads_test.cpp \
  -Wl,--wrap=fwrite -Wl,--wrap=fflush -Wl,--wrap=fclose \
  -Wl,--wrap=malloc -Wl,--wrap=setvbuf \
  -o /tmp/peppy-download-tests-sanitized
ASAN_OPTIONS=detect_leaks=0 /tmp/peppy-download-tests-sanitized
```

Leak detection is disabled for environments where LeakSanitizer cannot run
under the debugger or sandbox. The suite checks native handle cleanup explicitly.

These tests check control flow and integrity handling. They do not verify the PS4 certificate store, TLS negotiation, firmware compatibility, native symbol ABI, or console filesystem behavior. The production implementation explicitly enables peer/hostname/CA/date checks and SNI, and fails on TLS errors; no bypass callback is installed.
