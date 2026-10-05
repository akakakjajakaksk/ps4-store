# Download measurement tests

Run from `native/`:

```sh
g++ -std=c++11 -O2 -Wall -Wextra -Werror -D_FORTIFY_SOURCE=3 \
  tests/download_meter/meter_test.cpp -o /tmp/peppy-download-meter-tests
/tmp/peppy-download-meter-tests
```

The helper receives explicit monotonic microsecond timestamps and byte counters.
Fixtures check one-second sampling, weighted smoothing, same-timestamp updates,
three-second stalls, resumed progress, new downloads, backward counters/clocks,
unknown totals, completed transfers, long durations and full-width uint64 bounds.
An ETA is unavailable until a positive measured sample exists; final byte delivery
does not claim that package validation or installation finished.

Rates measure bytes delivered by the downloader, including its file writes and
optional hash verification. These tests do not measure Internet, Wi-Fi or PS4
throughput. The UI displays decimal MB/s and labels the remaining time as an
estimate. The PS4 supplies `sceKernelGetProcessTime()`; previews use explicit times.
