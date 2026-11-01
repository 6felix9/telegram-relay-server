// Unit tests for the RateLimiter lanes. No framework: each check aborts with a message.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "rate_limiter.hpp"

using namespace std::chrono_literals;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond << "\n"; \
            std::exit(1);                                                             \
        }                                                                             \
    } while (0)

// Wait until `n` callers are queued on the limiter (so arrival order is deterministic).
static void wait_for_waiting(const RateLimiter& l, int n) {
    for (int i = 0; i < 2000 && l.waiting() < n; ++i) std::this_thread::sleep_for(1ms);
    CHECK(l.waiting() == n);
}

struct Order {
    std::mutex mu;
    std::vector<std::string> done;
    void add(const std::string& s) { std::lock_guard<std::mutex> l(mu); done.push_back(s); }
};

static void test_normal_served_before_low_and_fifo_within_lane() {
    RateLimiter lim(20.0, 1.0);          // one token per 50 ms, burst 1
    CHECK(lim.acquire_for(0ms));         // drain the burst token
    Order order;
    std::vector<std::thread> ts;
    auto spawn = [&](const std::string& name, Lane lane, int expected_waiting) {
        ts.emplace_back([&, name, lane] {
            if (lim.acquire_for(5s, lane)) order.add(name);
        });
        wait_for_waiting(lim, expected_waiting);
    };
    spawn("low1", Lane::Low, 1);
    spawn("low2", Lane::Low, 2);
    spawn("norm1", Lane::Normal, 3);
    spawn("norm2", Lane::Normal, 4);
    for (auto& t : ts) t.join();
    CHECK((order.done == std::vector<std::string>{"norm1", "norm2", "low1", "low2"}));
}

static void test_timed_out_waiter_leaves_the_line() {
    RateLimiter lim(1.0, 1.0);
    CHECK(lim.acquire_for(0ms));
    CHECK(!lim.acquire_for(50ms, Lane::Normal));  // 1 token/s: cannot get one in 50 ms
    CHECK(lim.waiting() == 0);
    CHECK(!lim.acquire_for(50ms, Lane::Low));
    CHECK(lim.waiting() == 0);
}

static void test_low_waiter_not_blocked_forever_behind_departed_normal() {
    RateLimiter lim(20.0, 1.0);
    CHECK(lim.acquire_for(0ms));
    std::atomic<bool> low_ok{false};
    std::thread low([&] { low_ok = lim.acquire_for(5s, Lane::Low); });
    wait_for_waiting(lim, 1);
    // A normal caller arrives and gives up; the low caller must then be served.
    CHECK(!lim.acquire_for(10ms, Lane::Normal));
    low.join();
    CHECK(low_ok);
}

static void test_low_bucket_paces_and_bursts() {
    RateLimiter bucket(20.0 / 60.0, 3.0);  // 20/min, burst 3
    CHECK(bucket.acquire_for(0ms));
    CHECK(bucket.acquire_for(0ms));
    CHECK(bucket.acquire_for(0ms));
    CHECK(!bucket.acquire_for(100ms));      // next token is ~3 s away
}

static void test_pause_blocks_then_releases() {
    RateLimiter lim(100.0, 5.0);
    lim.pause_for(0.15);
    CHECK(lim.paused());
    CHECK(!lim.acquire_for(30ms));
    CHECK(lim.acquire_for(1s));
    CHECK(!lim.paused());
}

static void test_stop_releases_waiters() {
    RateLimiter lim(1.0, 1.0);
    CHECK(lim.acquire_for(0ms));
    std::atomic<bool> got{true};
    std::thread t([&] { got = lim.acquire(Lane::Low); });
    wait_for_waiting(lim, 1);
    lim.stop();
    t.join();
    CHECK(!got);
}

int main() {
    test_normal_served_before_low_and_fifo_within_lane();
    test_timed_out_waiter_leaves_the_line();
    test_low_waiter_not_blocked_forever_behind_departed_normal();
    test_low_bucket_paces_and_bursts();
    test_pause_blocks_then_releases();
    test_stop_releases_waiters();
    std::cout << "all rate limiter tests passed\n";
    return 0;
}
