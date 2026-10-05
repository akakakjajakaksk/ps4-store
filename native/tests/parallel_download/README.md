# Concurrent downloader host tests

Run from `native/` with GNU g++ and GNU ld:

```sh
g++ -std=c++11 -O2 -Wall -Wextra -Werror -pthread \
  -Itests/parallel_download/stubs tests/parallel_download/parallel_test.cpp \
  -Wl,--wrap=malloc -Wl,--wrap=close -Wl,--wrap=fsync \
  -Wl,--wrap=fclose -Wl,--wrap=fflush \
  -o /tmp/peppy-parallel-download-tests
/tmp/peppy-parallel-download-tests
```

The suite includes the production downloader and uses its public
`startDownload`, `downloadSnapshot` and `cancelDownload` entry points.
Native HTTP, TLS, network and pthread symbols are mocked. Each resource and
request has a unique handle; request headers, response positions, aborts
and lifecycle are protected by a host mutex. The worker uses actual host
threads and real descriptor-based `pwrite`/`pread`, `fsync` and file close.
Every run gets a unique temporary directory; no network access is needed.

The main fixture is a deterministic 32 MiB synthetic PS4 base package,
including magic, an exact Content ID, application type/flags, declared
package size and a strong ETag. The successful case requires two overlapping
native response reads, complementary exact byte ranges, `If-Range`, matching
ETag and exact Content-Length/Content-Range. It compares every published
byte against the original fixture and checks the complete final length.
With 16 KiB HTTP fragments, buffering produces 128 native writes instead of
one write per fragment. A 32 MiB + 123-byte case exercises both range tails.
These checks establish actual host concurrency and correctness; they do
not measure PS4 bandwidth or establish a twofold speed increase.

Before either range body is read or written, these cases fall back cleanly
to a single response: missing/weak ETag, ignored Range with HTTP200, HTTP503,
mismatched/duplicate ETag, wrong range offsets/total/Content-Length, second
network-pool failure, read-buffer allocation failure and joinable-thread
creation failure. Missing/weak ETag retains the original response. Other
fallback cases start a fresh response and never mix its bytes with range
data. All successful fallbacks still compare the complete output.

After range reading starts, these failures stop both requests and preserve
a prior completed package: native body error, early EOF, impossible read
count, native write error, zero/oversized write result, exhausted encoded
EINTR retries, fsync failure, close failure, native Fstat error, wrong file
size and nonregular file metadata. The Fstat mock writes an independent
120-byte native layout using mode offset8 and size offset72. A valid initial
GET followed by a ranged body with a different Content ID is rejected when
the assembled header is rechecked. Matching/mismatching optional SHA-256
checks read the assembled file in order. Successful interrupted reads and
writes and partial native writes preserve the complete fixture.

Cancellation waits until both lanes have already written their first
256 KiB blocks, then aborts both blocked native requests and joins the
worker before closing the descriptor or deleting native resources. Progress
is monotonic and never exceeds the expected package size. A transient join
failure keeps its original error, wakes the blocked worker, requires the
subsequent join and prevents publication. Resource mocks reject destruction
while a joinable worker is outstanding. Secondary close errors cannot
replace an earlier body/write failure.

A separate helper case writes two eight-byte lanes to offsets0 and
5 GiB + 123 of a real sparse file, then reads both locations and verifies
the full-width file length. It allocates only the helper's fixed buffers
and occupies at most a few filesystem blocks; no multi-gigabyte body is
generated or streamed.

Additional host checks:

```sh
g++ -std=c++11 -O2 -Wall -Wextra -Werror -D_FORTIFY_SOURCE=3 -pthread \
  -Itests/parallel_download/stubs tests/parallel_download/parallel_test.cpp \
  -Wl,--wrap=malloc -Wl,--wrap=close -Wl,--wrap=fsync \
  -Wl,--wrap=fclose -Wl,--wrap=fflush \
  -o /tmp/peppy-parallel-download-tests-fortify
/tmp/peppy-parallel-download-tests-fortify
g++ -std=c++11 -O1 -g -Wall -Wextra -Werror -pthread \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Itests/parallel_download/stubs tests/parallel_download/parallel_test.cpp \
  -Wl,--wrap=malloc -Wl,--wrap=close -Wl,--wrap=fsync \
  -Wl,--wrap=fclose -Wl,--wrap=fflush \
  -o /tmp/peppy-parallel-download-tests-sanitized
ASAN_OPTIONS=detect_leaks=0 /tmp/peppy-parallel-download-tests-sanitized
```

Leak detection is disabled where LeakSanitizer cannot run under the
sandbox. Explicit assertions still check every native mock handle and
joinable host thread. These tests do not verify the native PS4 symbol ABI,
certificate store, Archive's current Range/ETag support, real filesystem
behavior, firmware compatibility or console throughput.
