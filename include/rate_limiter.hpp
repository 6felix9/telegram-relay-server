#pragma once

#include <chrono>
#include <mutex>
#include <condition_variable>
#include <algorithm>

class RateLimiter {
public:
    // rate_per_second: maximum sustained tokens (calls) allowed per second
    // burst_capacity: maximum tokens bucket can accumulate during idle periods (-1 defaults to rate_per_second)
    explicit RateLimiter(double rate_per_second = 20.0, double burst_capacity = -1.0)
        : rate_per_second_(rate_per_second <= 0.0 ? 20.0 : rate_per_second),
          capacity_(burst_capacity > 0 ? burst_capacity : rate_per_second_),
          tokens_(capacity_),
          last_refill_(std::chrono::steady_clock::now()),
          stopped_(false) {}

    ~RateLimiter() {
        stop();
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
        }
        cv_.notify_all();
    }

    // Blocks until a rate limit token is acquired.
    // Returns true if token acquired, false if stopped/shutting down.
    bool acquire() {
        std::unique_lock<std::mutex> lock(mutex_);

        while (!stopped_) {
            refill();

            if (tokens_ >= 1.0) {
                tokens_ -= 1.0;
                return true;
            }

            // Calculate precise sleep duration needed for 1 token to become available
            double tokens_needed = 1.0 - tokens_;
            double seconds_to_wait = tokens_needed / rate_per_second_;
            auto wait_duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::duration<double>(seconds_to_wait)
            );

            // Wait until duration elapses or stop() is signaled
            cv_.wait_for(lock, wait_duration);
        }

        return false;
    }

    double rate() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return rate_per_second_;
    }

    double available_tokens() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return tokens_;
    }

private:
    void refill() {
        auto now = std::chrono::steady_clock::now();
        std::chrono::duration<double> elapsed = now - last_refill_;
        last_refill_ = now;

        tokens_ = std::min(capacity_, tokens_ + elapsed.count() * rate_per_second_);
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    double rate_per_second_;
    double capacity_;
    double tokens_;
    std::chrono::steady_clock::time_point last_refill_;
    bool stopped_{false};
};
