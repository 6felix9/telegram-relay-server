#pragma once

#include <vector>
#include <thread>
#include <iostream>
#include <memory>
#include <atomic>
#include <chrono>
#include "job_queue.hpp"
#include "rate_limiter.hpp"

class WorkerPool {
public:
    explicit WorkerPool(std::shared_ptr<JobQueue> queue,
                        std::shared_ptr<RateLimiter> rate_limiter = nullptr,
                        size_t worker_count = 3)
        : queue_(std::move(queue)),
          rate_limiter_(std::move(rate_limiter)),
          worker_count_(worker_count == 0 ? 1 : worker_count),
          running_(false) {}

    ~WorkerPool() {
        stop();
    }

    void start() {
        if (running_) {
            return;
        }
        running_ = true;
        workers_.reserve(worker_count_);
        for (size_t i = 0; i < worker_count_; ++i) {
            workers_.emplace_back(&WorkerPool::worker_loop, this, i + 1);
        }
    }

    void stop() {
        if (!running_) {
            return;
        }
        running_ = false;

        // Stop rate limiter and queue to release all waiting workers
        if (rate_limiter_) {
            rate_limiter_->stop();
        }
        if (queue_) {
            queue_->stop();
        }

        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers_.clear();
    }

    size_t worker_count() const {
        return worker_count_;
    }

    bool is_running() const {
        return running_;
    }

    std::shared_ptr<RateLimiter> rate_limiter() const {
        return rate_limiter_;
    }

private:
    void worker_loop(size_t worker_id) {
        {
            std::lock_guard<std::mutex> lock(log_mutex_);
            std::cout << "[Worker #" << worker_id << "] Started and listening for jobs." << std::endl;
        }

        Job job;
        while (queue_->pop(job)) {
            process_job(worker_id, job);
        }

        {
            std::lock_guard<std::mutex> lock(log_mutex_);
            std::cout << "[Worker #" << worker_id << "] Stopped." << std::endl;
        }
    }

    void process_job(size_t worker_id, const Job& job) {
        // 1. Acquire global rate limiter token before initiating outgoing call
        if (rate_limiter_) {
            if (!rate_limiter_->acquire()) {
                return; // Server is shutting down
            }
        }

        {
            std::lock_guard<std::mutex> lock(log_mutex_);

            // Get current timestamp using thread-safe reentrant localtime_r
            auto now = std::chrono::system_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
            std::time_t now_c = std::chrono::system_clock::to_time_t(now);
            struct tm tm_buf;
#if defined(_WIN32)
            localtime_s(&tm_buf, &now_c);
#else
            localtime_r(&now_c, &tm_buf);
#endif
            char time_buf[16];
            std::strftime(time_buf, sizeof(time_buf), "%H:%M:%S", &tm_buf);

            std::cout << "[" << time_buf << "." << (ms.count() < 10 ? "00" : (ms.count() < 100 ? "0" : "")) << ms.count()
                      << "] [Worker #" << worker_id << "] 🚀 OUTGOING CALL message_id=" << job.message_id
                      << " (type=" << to_string(job.type);


            if (!job.text.empty()) {
                std::cout << ", text=\"" << job.text << "\"";
            }
            if (!job.file_id.empty()) {
                std::cout << ", file_id=\"" << job.file_id << "\"";
            }
            if (rate_limiter_) {
                std::cout << ", rate_limit=" << rate_limiter_->rate() << "/s";
            }
            std::cout << ")" << std::endl;
        }

        // Outbound action dispatch based on job.type (e.g. Telegram Bot API call)
        switch (job.type) {
            case JobType::SendText:
                break;
            case JobType::SendPhoto:
                break;
            case JobType::SendVoice:
                break;
            case JobType::SendVideoNote:
                break;
            case JobType::SendSticker:
                break;
            case JobType::Delete:
                break;
        }
    }

    std::shared_ptr<JobQueue> queue_;
    std::shared_ptr<RateLimiter> rate_limiter_;
    size_t worker_count_;
    std::atomic<bool> running_{false};
    std::vector<std::thread> workers_;
    mutable std::mutex log_mutex_;
};
