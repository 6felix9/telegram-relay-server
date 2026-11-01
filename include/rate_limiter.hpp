#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>

// Priority of a waiter. When a token becomes available, every waiting Normal caller is
// served before any waiting Low caller. Within a lane, callers are served in arrival order.
enum class Lane { Normal = 0, Low = 1 };

class RateLimiter {
public:
    using Clock = std::chrono::steady_clock;

    // rate_per_second: maximum sustained tokens (calls) allowed per second
    // burst_capacity: maximum tokens bucket can accumulate during idle periods (-1 defaults to rate_per_second)
    explicit RateLimiter(double rate_per_second = 20.0, double burst_capacity = -1.0)
        : rate_per_second_(rate_per_second <= 0.0 ? 20.0 : rate_per_second),
          capacity_(burst_capacity > 0 ? burst_capacity : rate_per_second_),
          tokens_(capacity_),
          last_refill_(Clock::now()),
          paused_until_(Clock::now()),
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
    bool acquire(Lane lane = Lane::Normal) {
        return acquire_until(Clock::time_point::max(), lane);
    }

    // Like acquire(), but gives up after `max_wait`.
    // Returns true if a token was acquired, false on timeout or shutdown.
    bool acquire_for(std::chrono::milliseconds max_wait, Lane lane = Lane::Normal) {
        return acquire_until(Clock::now() + max_wait, lane);
    }

    // Like acquire_for(), with an absolute deadline (lets a caller share one deadline
    // across several limiters).
    bool acquire_by(Clock::time_point deadline, Lane lane = Lane::Normal) {
        return acquire_until(deadline, lane);
    }

    // Stop granting tokens for `seconds` (used when Telegram answers 429 with
    // retry_after). Callers already waiting simply wait longer. Pauses never
    // shorten an existing pause.
    void pause_for(double seconds) {
        if (seconds <= 0.0) return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto until = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(seconds));
            if (until > paused_until_) paused_until_ = until;
            tokens_ = 0.0;                // no burst credit once the pause ends:
            last_refill_ = paused_until_; // refill restarts when the pause ends
        }
        cv_.notify_all();
    }

    bool paused() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return Clock::now() < paused_until_;
    }

    double rate() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return rate_per_second_;
    }

    double available_tokens() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return tokens_;
    }

    // Number of callers currently blocked waiting for a token.
    int waiting() const {
        return waiting_[0].load() + waiting_[1].load();
    }

    int waiting(Lane lane) const {
        return waiting_[static_cast<int>(lane)].load();
    }

private:
    bool acquire_until(Clock::time_point deadline, Lane lane) {
        const int lane_idx = static_cast<int>(lane);
        std::unique_lock<std::mutex> lock(mutex_);

        const uint64_t ticket = next_ticket_++;
        queues_[lane_idx].push_back(ticket);
        waiting_[lane_idx]++;

        // Runs (under the lock) however we leave: take our place out of the queue and let
        // the new head of the line re-check.
        struct Guard {
            RateLimiter& self; int lane_idx; uint64_t ticket;
            ~Guard() {
                auto& q = self.queues_[lane_idx];
                for (auto it = q.begin(); it != q.end(); ++it) {
                    if (*it == ticket) { q.erase(it); break; }
                }
                self.waiting_[lane_idx]--;
                self.cv_.notify_all();
            }
        } guard{*this, lane_idx, ticket};

        while (!stopped_) {
            auto now = Clock::now();

            // Only the head of the line may take a token, and a Low caller is never the
            // head while any Normal caller is waiting.
            const bool is_head = queues_[lane_idx].front() == ticket &&
                                 (lane == Lane::Normal || queues_[0].empty());

            if (now >= paused_until_) {
                refill(now);
                if (is_head && tokens_ >= 1.0) {
                    tokens_ -= 1.0;
                    return true;
                }
            }

            if (now >= deadline) {
                return false;
            }

            Clock::time_point wake;
            if (!is_head) {
                // Not our turn: sleep until the line changes (notify_all) or we time out.
                wake = now + std::chrono::hours(1);
            } else if (now < paused_until_) {
                wake = paused_until_;
            } else {
                // Time until a token could be available
                double seconds_to_wait = (1.0 - tokens_) / rate_per_second_;
                wake = now + std::chrono::duration_cast<Clock::duration>(
                    std::chrono::duration<double>(seconds_to_wait));
            }
            if (wake > deadline) wake = deadline;

            cv_.wait_until(lock, wake);
        }

        return false;
    }

    void refill(Clock::time_point now) {
        std::chrono::duration<double> elapsed = now - last_refill_;
        last_refill_ = now;
        tokens_ = std::min(capacity_, tokens_ + elapsed.count() * rate_per_second_);
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    double rate_per_second_;
    double capacity_;
    double tokens_;
    Clock::time_point last_refill_;
    Clock::time_point paused_until_;
    std::deque<uint64_t> queues_[2];  // tickets waiting, per lane, in arrival order
    uint64_t next_ticket_{0};
    std::atomic<int> waiting_[2]{{0}, {0}};
    bool stopped_{false};
};
