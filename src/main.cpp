#include <iostream>
#include <string>
#include <csignal>
#include <cstdlib>
#include <memory>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include "job.hpp"
#include "job_queue.hpp"
#include "rate_limiter.hpp"
#include "worker_pool.hpp"

using json = nlohmann::json;

// Global references for signal handling
httplib::Server svr;
std::shared_ptr<WorkerPool> g_worker_pool = nullptr;

void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        svr.stop();
    }
}


int main(int argc, char* argv[]) {
    // Setup signal handling
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // 1. Configure Port (CLI argument 1, or PORT env, default 8080)
    int port = 8080;
    if (const char* env_port = std::getenv("PORT")) {
        port = std::stoi(env_port);
    }
    if (argc > 1) {
        port = std::stoi(argv[1]);
    }

    // 2. Configure Worker Count (CLI argument 2, or WORKERS / WORKER_COUNT env, default 3)
    size_t worker_count = 3;
    if (const char* env_workers = std::getenv("WORKERS")) {
        worker_count = std::stoul(env_workers);
    } else if (const char* env_worker_count = std::getenv("WORKER_COUNT")) {
        worker_count = std::stoul(env_worker_count);
    }
    if (argc > 2) {
        worker_count = std::stoul(argv[2]);
    }

    // 3. Configure Rate Limit (CLI argument 3, or RATE_LIMIT env, default 20 calls/sec)
    double rate_limit = 20.0;
    if (const char* env_rate = std::getenv("RATE_LIMIT")) {
        rate_limit = std::stod(env_rate);
    }
    if (argc > 3) {
        rate_limit = std::stod(argv[3]);
    }

    // 4. Initialize Queue, Rate Limiter, and Worker Pool
    auto job_queue = std::make_shared<JobQueue>();
    auto rate_limiter = std::make_shared<RateLimiter>(rate_limit);
    g_worker_pool = std::make_shared<WorkerPool>(job_queue, rate_limiter, worker_count);
    g_worker_pool->start();

    // Health check endpoint (includes worker, queue, and rate limit stats)
    svr.Get("/health", [job_queue, rate_limiter, worker_count](const httplib::Request&, httplib::Response& res) {
        json response = {
            {"status", "ok"},
            {"workers", worker_count},
            {"rate_limit_per_second", rate_limiter->rate()},
            {"pending_jobs", job_queue->size()}
        };
        res.set_content(response.dump(2), "application/json");
    });

    // Relay endpoint handler: pushes job onto queue and returns immediately
    auto handle_relay_request = [job_queue](const httplib::Request& req, httplib::Response& res) {
        json payload;
        try {
            payload = json::parse(req.body);
        } catch (const json::parse_error& e) {
            std::cerr << "[ERROR] JSON parse failed: " << e.what() << std::endl;
            json err_res = {
                {"accepted", false},
                {"error", "Malformed JSON"}
            };
            res.status = 400;
            res.set_content(err_res.dump(2), "application/json");
            return;
        }

        // Deserialize and validate into Job struct
        try {
            Job job = payload.get<Job>();

            // Enqueue job for background workers
            job_queue->push(std::move(job));

            // Return accepted response immediately (< 1ms)
            json success_res = {
                {"accepted", true}
            };
            res.status = 200;
            res.set_content(success_res.dump(2), "application/json");
        } catch (const std::exception& e) {
            std::cerr << "[ERROR] Invalid job payload: " << e.what() << std::endl;
            json err_res = {
                {"accepted", false},
                {"error", std::string("Invalid job payload: ") + e.what()}
            };
            res.status = 400;
            res.set_content(err_res.dump(2), "application/json");
        }
    };

    // Support both root "/" and "/relay" / "/message" endpoints
    svr.Post("/", handle_relay_request);
    svr.Post("/relay", handle_relay_request);
    svr.Post("/message", handle_relay_request);

    std::cout << "===========================================" << std::endl;
    std::cout << " Telegram Relay Server running on port " << port << std::endl;
    std::cout << " Worker threads active: " << worker_count << std::endl;


    std::cout << " Global Rate Limit:     " << rate_limit << " calls/sec" << std::endl;
    std::cout << " Endpoints:" << std::endl;
    std::cout << "   GET  /health" << std::endl;
    std::cout << "   POST / (or /relay, /message)" << std::endl;
    std::cout << " Press Ctrl+C to stop." << std::endl;
    std::cout << "===========================================" << std::endl;

    if (!svr.listen("0.0.0.0", port)) {
        std::cerr << "[FATAL] Failed to bind to 0.0.0.0:" << port << std::endl;
        if (g_worker_pool) {
            g_worker_pool->stop();
        }
        return 1;
    }

    if (g_worker_pool) {
        g_worker_pool->stop();
    }

    return 0;
}
