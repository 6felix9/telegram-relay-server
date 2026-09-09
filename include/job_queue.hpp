#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>
#include <cstddef>
#include "job.hpp"

class JobQueue {
public:
    JobQueue() : stopping_(false) {}

    ~JobQueue() {
        stop();
    }

    // Push a new job into the queue and notify one waiting worker thread
    void push(Job job) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) {
                return;
            }
            queue_.push(std::move(job));
        }
        cv_.notify_one();
    }

    // Pop a job from the queue.
    // Blocks if queue is empty until a job arrives or stop() is called.
    // Returns true if a job was retrieved, false if queue is stopping and empty.
    bool pop(Job& job) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]() {
            return stopping_ || !queue_.empty();
        });

        if (queue_.empty()) {
            return false;
        }

        job = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    // Signal all waiting threads to wake up and stop accepting new jobs
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_all();
    }

    bool is_stopping() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stopping_;
    }

    // Get current number of pending jobs in queue
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<Job> queue_;
    bool stopping_{false};
};
