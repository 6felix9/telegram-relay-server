#include <iostream>
#include <string>
#include <csignal>
#include <cstdlib>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include "job.hpp"

using json = nlohmann::json;

// Global server instance for signal handling
httplib::Server svr;

void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        std::cout << "\n[INFO] Gracefully shutting down server..." << std::endl;
        svr.stop();
    }
}

int main(int argc, char* argv[]) {
    // Setup signal handling
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Determine port from environment or CLI argument (default: 8080)
    int port = 8080;
    if (const char* env_port = std::getenv("PORT")) {
        port = std::stoi(env_port);
    }
    if (argc > 1) {
        port = std::stoi(argv[1]);
    }

    // Health check endpoint
    svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        json response = {
            {"status", "ok"}
        };
        res.set_content(response.dump(2), "application/json");
    });

    // Relay endpoint handler
    auto handle_relay_request = [](const httplib::Request& req, httplib::Response& res) {
        std::cout << "[INFO] Incoming POST request from " << req.remote_addr << std::endl;

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

            std::cout << "[INFO] Received Job: "
                      << "chat_id=" << job.chat_id
                      << ", message_id=" << job.message_id
                      << ", reply_to_message_id=" << job.reply_to_message_id
                      << ", type=" << to_string(job.type);

            if (!job.text.empty()) {
                std::cout << ", text=\"" << job.text << "\"";
            }
            if (!job.file_id.empty()) {
                std::cout << ", file_id=\"" << job.file_id << "\"";
            }
            std::cout << std::endl;

            // Prepare success response
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
    std::cout << " Relay Engine Server running on port " << port << std::endl;
    std::cout << " Endpoints:" << std::endl;
    std::cout << "   GET  /health" << std::endl;
    std::cout << "   POST / (or /relay, /message)" << std::endl;
    std::cout << " Press Ctrl+C to stop." << std::endl;
    std::cout << "===========================================" << std::endl;

    if (!svr.listen("0.0.0.0", port)) {
        std::cerr << "[FATAL] Failed to bind to 0.0.0.0:" << port << std::endl;
        return 1;
    }

    return 0;
}
