#pragma once

#include <vector>
#include <thread>
#include <iostream>
#include <memory>
#include <atomic>
#include <chrono>
#include <random>
#include "job_queue.hpp"

class WorkerPool {
public:
    explicit WorkerPool(std::shared_ptr<JobQueue> queue, size_t worker_count = 3)
        : queue_(std::move(queue)), worker_count_(worker_count == 0 ? 1 : worker_count), running_(false) {}

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
        // Generate simulated network latency between 100ms and 500ms
        static thread_local std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<int> dist(100, 500);
        int simulated_latency_ms = dist(rng);

        {
            std::lock_guard<std::mutex> lock(log_mutex_);
            std::cout << "[Worker #" << worker_id << "] ⏳ START Job: "
                      << "chat_id=" << job.chat_id
                      << ", message_id=" << job.message_id
                      << ", type=" << to_string(job.type);

            if (!job.text.empty()) {
                std::cout << ", text=\"" << job.text << "\"";
            }
            if (!job.file_id.empty()) {
                std::cout << ", file_id=\"" << job.file_id << "\"";
            }
            std::cout << " (simulating " << simulated_latency_ms << "ms network call...)"
                      << std::endl;
        }

        // Simulate outbound network request delay (e.g. calling Telegram Bot API)
        std::this_thread::sleep_for(std::chrono::milliseconds(simulated_latency_ms));

        {
            std::lock_guard<std::mutex> lock(log_mutex_);
            std::cout << "[Worker #" << worker_id << "] ✅ FINISHED Job: "
                      << "message_id=" << job.message_id
                      << " in " << simulated_latency_ms << "ms"
                      << std::endl;
        }

        // Dispatched action placeholder based on job.type
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
    size_t worker_count_;
    std::atomic<bool> running_{false};
    std::vector<std::thread> workers_;
    mutable std::mutex log_mutex_;
};

