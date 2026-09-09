#include <iostream>
#include <string>
#include <csignal>
#include <cstdlib>
#include <httplib.h>
#include <nlohmann/json.hpp>

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

        // Validate required fields
        if (!payload.contains("chat_id") || !payload.contains("message_id") || !payload.contains("type")) {
            std::cerr << "[WARN] Missing required fields in payload" << std::endl;
            json err_res = {
                {"accepted", false},
                {"error", "Required fields missing. Expected: chat_id, message_id, type"}
            };
            res.status = 400;
            res.set_content(err_res.dump(2), "application/json");
            return;
        }

        // Extract and log details
        try {
            int64_t chat_id = payload["chat_id"].get<int64_t>();
            int64_t message_id = payload["message_id"].get<int64_t>();
            std::string type = payload["type"].get<std::string>();

            std::cout << "[INFO] Accepted telegram event:"
                      << " chat_id=" << chat_id
                      << ", message_id=" << message_id
                      << ", type=\"" << type << "\""
                      << std::endl;

            // Prepare success response
            json success_res = {
                {"accepted", true}
            };
            res.status = 200;
            res.set_content(success_res.dump(2), "application/json");
        } catch (const json::exception& e) {
            std::cerr << "[ERROR] Field type mismatch: " << e.what() << std::endl;
            json err_res = {
                {"accepted", false},
                {"error", "Invalid field types for chat_id, message_id, or type"}
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
