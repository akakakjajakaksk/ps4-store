#ifndef PEPPY_DOWNLOAD_METER_H
#define PEPPY_DOWNLOAD_METER_H

#include <stdint.h>

struct DownloadMeasurement {
    double bytesPerSecond;
    uint64_t remainingSeconds;
    bool rateAvailable, etaAvailable, finalizing;
};

// UI-owned measurement of bytes delivered by the downloader. Times are
// monotonic microseconds; no socket, filesystem or shared worker lock is used.
class DownloadMeter {
public:
    DownloadMeter() { reset(); }

    void reset() {
        tracking_ = false;
        sampleBytes_ = observedBytes_ = total_ = 0;
        sampleTime_ = observedTime_ = lastAdvance_ = 0;
        count_ = next_ = 0;
        value_ = DownloadMeasurement{0.0, 0, false, false, false};
        for (unsigned i = 0; i < 3; ++i) samples_[i] = Sample{0, 0};
    }

    DownloadMeasurement measurement() const { return value_; }

    // True requests a UI refresh, including when an unchanged counter stalls.
    bool update(bool running, uint64_t bytes, uint64_t total, uint64_t now) {
        if (!running) {
            bool changed = tracking_;
            reset();
            return changed;
        }
        if (!tracking_ || bytes < observedBytes_ || total != total_ || now < observedTime_) {
            reset();
            tracking_ = true;
            sampleBytes_ = observedBytes_ = bytes;
            sampleTime_ = observedTime_ = lastAdvance_ = now;
            total_ = total;
            value_.finalizing = total && bytes >= total;
            return true;
        }
        if (bytes > observedBytes_) lastAdvance_ = now;
        observedBytes_ = bytes;
        observedTime_ = now;
        bool finalizing = total && bytes >= total;
        bool changed = finalizing != value_.finalizing;
        value_.finalizing = finalizing;
        if (finalizing) value_.etaAvailable = false;

        uint64_t elapsed = now - sampleTime_;
        if (elapsed < 1000000) return changed;
        uint64_t delta = bytes - sampleBytes_;
        sampleBytes_ = bytes;
        sampleTime_ = now;
        // Do not dilute the first data sample with DNS/page-resolution waits.
        if (!delta && !value_.rateAvailable) return changed;
        samples_[next_] = Sample{delta, elapsed};
        next_ = (next_ + 1) % 3;
        if (count_ < 3) ++count_;
        double sumBytes = 0.0, sumMicros = 0.0;
        for (unsigned i = 0; i < count_; ++i) {
            sumBytes += static_cast<double>(samples_[i].bytes);
            sumMicros += static_cast<double>(samples_[i].micros);
        }
        // Convert before multiplying: uint64 counters must never overflow.
        value_.bytesPerSecond = (sumBytes / sumMicros) * 1000000.0;
        value_.rateAvailable = true;
        if (!delta && now - lastAdvance_ >= 3000000) {
            value_.bytesPerSecond = 0.0;
            count_ = next_ = 0;
        }
        value_.etaAvailable = false;
        value_.remainingSeconds = 0;
        if (!finalizing && total > bytes && value_.bytesPerSecond > 0.0) {
            double seconds = static_cast<double>(total - bytes) / value_.bytesPerSecond;
            // UINT64_MAX rounds to 2^64 as a double. Reject that boundary
            // before converting, then round up without overflowing.
            const double limit = 18446744073709551616.0;
            if (seconds < limit) {
                uint64_t whole = static_cast<uint64_t>(seconds);
                if (static_cast<double>(whole) < seconds) ++whole;
                value_.remainingSeconds = whole;
                value_.etaAvailable = true;
            }
        }
        return true;
    }

private:
    struct Sample { uint64_t bytes, micros; };
    Sample samples_[3];
    uint64_t sampleBytes_, observedBytes_, total_;
    uint64_t sampleTime_, observedTime_, lastAdvance_;
    unsigned count_, next_;
    bool tracking_;
    DownloadMeasurement value_;
};

#endif
