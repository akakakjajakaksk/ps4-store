#include "../../download_meter.h"

#include <assert.h>
#include <cmath>
#include <stdio.h>

static void expectRate(const DownloadMeter& meter, double rate) {
    DownloadMeasurement value = meter.measurement();
    assert(value.rateAvailable && std::isfinite(value.bytesPerSecond));
    assert(std::fabs(value.bytesPerSecond - rate) <= 0.000001 * (rate + 1.0));
}

static void measuredRates() {
    DownloadMeter meter;
    assert(meter.update(true, 0, 10000000, 0));
    assert(!meter.update(true, 500000, 10000000, 500000));
    assert(!meter.measurement().rateAvailable && !meter.measurement().etaAvailable);
    assert(meter.update(true, 1000000, 10000000, 1000000));
    expectRate(meter, 1000000.0);
    assert(meter.measurement().etaAvailable && meter.measurement().remainingSeconds == 9);

    assert(meter.update(true, 3000000, 10000000, 2000000));
    expectRate(meter, 1500000.0);
    assert(meter.measurement().remainingSeconds == 5); // Ceiling of 7 / 1.5.
    assert(!meter.update(true, 4000000, 10000000, 2000000));
    expectRate(meter, 1500000.0); // Same-time bytes cannot inflate speed.
    assert(meter.update(true, 5000000, 10000000, 3000000));
    expectRate(meter, 5000000.0 / 3.0); // Includes pending bytes from the same timestamp.
    assert(meter.measurement().remainingSeconds == 3);

    meter.reset();
    meter.update(true, 0, 10 * 1048576ULL, 0);
    meter.update(true, 1048576, 10 * 1048576ULL, 1000000);
    expectRate(meter, 1048576.0); // Display conversion to decimal MB/s is 1.048576.
    assert(meter.measurement().remainingSeconds == 9);

    meter.reset();
    meter.update(true, 0, 10000000, 0);
    meter.update(true, 1000000, 10000000, 1000000);
    meter.update(true, 3000000, 10000000, 6000000);
    expectRate(meter, 500000.0); // 3 MB over 6 s, not the mean of sample rates.
    assert(meter.measurement().remainingSeconds == 14);
}

static void stallsAndRestart() {
    DownloadMeter meter;
    meter.update(true, 0, 10000000, 0);
    meter.update(true, 1000000, 10000000, 1000000);
    meter.update(true, 1000000, 10000000, 2000000);
    expectRate(meter, 500000.0);
    meter.update(true, 1000000, 10000000, 3000000);
    expectRate(meter, 1000000.0 / 3.0);
    assert(meter.update(true, 1000000, 10000000, 4000000));
    expectRate(meter, 0.0);
    assert(!meter.measurement().etaAvailable);
    assert(meter.update(true, 1000000, 10000000, 5000000)); // Redraw a stalled meter.
    expectRate(meter, 0.0);
    meter.update(true, 2000000, 10000000, 6000000);
    expectRate(meter, 1000000.0); // Old samples were discarded after the stall.
    assert(meter.measurement().remainingSeconds == 8);

    meter.update(true, 100, 10000000, 6100000); // Byte counter decreases on retry.
    assert(!meter.measurement().rateAvailable);
    meter.update(true, 1100, 10000000, 7100000);
    expectRate(meter, 1000.0);
    meter.update(true, 1100, 20000000, 7200000); // A different transfer total.
    assert(!meter.measurement().rateAvailable);
    meter.update(true, 2100, 20000000, 8200000);
    expectRate(meter, 1000.0);
    meter.update(true, 2100, 20000000, 100); // Monotonic clock regression.
    assert(!meter.measurement().rateAvailable);
    meter.update(true, 3100, 20000000, 1000100);
    expectRate(meter, 1000.0);

    meter.reset(); // Accepted new download of the same app/size.
    assert(!meter.measurement().rateAvailable && !meter.measurement().etaAvailable);
    meter.update(true, 0, 20000000, 9000000);
    meter.update(true, 1000000, 20000000, 10000000);
    expectRate(meter, 1000000.0);
}

static void preparationAndCompletion() {
    DownloadMeter meter;
    meter.update(true, 0, 10000000, 0);
    meter.update(true, 0, 10000000, 1000000);
    meter.update(true, 0, 10000000, 2000000);
    assert(!meter.measurement().rateAvailable);
    meter.update(true, 1000000, 10000000, 3000000);
    expectRate(meter, 1000000.0); // DNS/page wait did not enter the smoothing window.
    meter.update(true, 10000000, 10000000, 3200000);
    assert(meter.measurement().finalizing && !meter.measurement().etaAvailable);
    assert(meter.update(false, 10000000, 10000000, 3300000)); // DONE / FAILED / CANCELLED.
    assert(!meter.measurement().rateAvailable && !meter.measurement().finalizing);
    assert(!meter.update(false, 0, 0, 3400000)); // Idle must not force redraws.

    meter.update(true, 0, 0, 3500000);
    meter.update(true, 1000000, 0, 4500000);
    expectRate(meter, 1000000.0);
    assert(!meter.measurement().etaAvailable); // Unknown total, still measured speed.
}

static void largeCountersAndDurations() {
    DownloadMeter meter;
    const uint64_t total = 256ULL * 1024 * 1024 * 1024;
    meter.update(true, 0, total, 0);
    meter.update(true, 5ULL * 1024 * 1024 * 1024, total, 3600000000ULL);
    expectRate(meter, (5.0 * 1024 * 1024 * 1024) / 3600.0);
    assert(meter.measurement().etaAvailable && meter.measurement().remainingSeconds == 180720);

    meter.reset();
    meter.update(true, 0, UINT64_MAX, 0);
    meter.update(true, UINT64_MAX - 1, UINT64_MAX, 1000000);
    expectRate(meter, static_cast<double>(UINT64_MAX - 1));
    assert(meter.measurement().etaAvailable && meter.measurement().remainingSeconds == 1);

    meter.reset();
    meter.update(true, 0, UINT64_MAX, 0);
    meter.update(true, 1, UINT64_MAX, UINT64_MAX);
    assert(meter.measurement().rateAvailable && std::isfinite(meter.measurement().bytesPerSecond));
    assert(meter.measurement().bytesPerSecond > 0.0 && !meter.measurement().etaAvailable);
}

int main() {
    measuredRates();
    stallsAndRestart();
    preparationAndCompletion();
    largeCountersAndDurations();
    puts("Measured download rate, ETA, stalls, reset, completion and 64-bit bounds passed.");
    return 0;
}
