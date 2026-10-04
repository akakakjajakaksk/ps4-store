# Downloader host tests

Run from `native/` on Linux with GNU g++ and GNU ld:

```sh
g++ -std=c++11 -O2 -Wall -Wextra -Werror -pthread \
  -Itests/downloads/stubs tests/downloads/downloads_test.cpp \
  -Wl,--wrap=fwrite -Wl,--wrap=fflush -Wl,--wrap=fclose \
  -o /tmp/peppy-download-tests
/tmp/peppy-download-tests
```

The shim preserves the OpenOrbis 0.5.4 declarations needed by the downloader, including incomplete SDK declarations handled through asm aliases. It replaces network and thread calls with deterministic host mocks and uses a unique temporary directory per run. It does not contact GitHub or a PS4.

Coverage includes four SHA-256 NIST vectors, fragmented PKG magic, normal and redirected transfers, strict HTTPS/host/filename checks, redirect limits, HTTP/TLS errors, truncation and overflow, missing content lengths, mismatched digests, write/flush/close failures, preservation of an existing complete file on a failed replacement, detached worker creation, one transfer at a time, cancellation, and handle/partial-file cleanup. Initialization checks cover already-loaded modules, a failed load with a confirmed loaded probe, failed module loading, NetCtl initialization/state errors, bounded local-IP readiness, cancellation during readiness, pool errors, individual timeout errors, original native/SSL diagnostics, and repeated download cleanup. Error fixture numbers are mock values, not a whitelist of hardware errors.

The transfer writes a numeric diagnostic log at `/data/peppy-store/downloads/download.log` on a PS4 (inside the unique test directory on the host). Tests check that the log contains no URLs or signed tokens.

These tests check control flow and integrity handling. They do not verify the PS4 certificate store, TLS negotiation, firmware compatibility, native symbol ABI, or console filesystem behavior. The production implementation explicitly enables peer/hostname/CA/date checks and SNI, and fails on TLS errors; no bypass callback is installed.
