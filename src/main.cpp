// telegram-relay-server
//
// A transparent, synchronous proxy for the Telegram Bot API. Clients call
//   POST /bot<token-slot>/<method>
// exactly as they would call api.telegram.org. The relay waits for a global
// rate-limit token, forwards the call, and returns Telegram's response
// (status code and body) unchanged. See specs/relay-proxy.md.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include "rate_limiter.hpp"

using json = nlohmann::json;

namespace {

std::atomic<bool> g_stop{false};
std::mutex g_log_mutex;

void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        g_stop = true;  // async-signal-safe; a watcher thread does the real shutdown
    }
}

template <typename Fn>
void log_line(Fn&& write_body) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    write_body(std::cout);
    std::cout << std::endl;
}

// Pool of persistent HTTPS connections to Telegram. A fresh client per call paid DNS + TCP
// + TLS handshakes (~1s+) every time. Requests land on arbitrary server threads, so the
// pool is shared rather than thread_local. A client is checked out for one call (a long
// poll holds one for its duration) and returned afterwards; httplib reconnects on its own
// if Telegram closed an idle connection.
class TelegramClientPool {
public:
    class Lease {
    public:
        Lease(TelegramClientPool& pool, std::unique_ptr<httplib::Client> c)
            : pool_(pool), client_(std::move(c)) {}
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        ~Lease() { pool_.release(std::move(client_)); }
        httplib::Client& operator*() { return *client_; }
        httplib::Client* operator->() { return client_.get(); }
    private:
        TelegramClientPool& pool_;
        std::unique_ptr<httplib::Client> client_;
    };

    Lease acquire() {
        std::unique_ptr<httplib::Client> c;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!idle_.empty()) {
                c = std::move(idle_.back());
                idle_.pop_back();
            }
        }
        if (!c) {
            c = std::make_unique<httplib::Client>("https://api.telegram.org");
            c->set_keep_alive(true);
            c->set_tcp_nodelay(true);  // don't let Nagle hold the body back behind the headers
            c->set_connection_timeout(5, 0);
            c->set_write_timeout(15, 0);
        }
        return Lease(*this, std::move(c));
    }

private:
    void release(std::unique_ptr<httplib::Client> c) {
        std::lock_guard<std::mutex> lock(mu_);
        idle_.push_back(std::move(c));
    }
    std::mutex mu_;
    std::vector<std::unique_ptr<httplib::Client>> idle_;
};

TelegramClientPool g_client_pool;

std::string env_or(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : fallback;
}

// Telegram-style error body so clients (python-telegram-bot) can parse it.
void set_tg_error(httplib::Response& res, int status, const std::string& description) {
    json body = {
        {"ok", false},
        {"error_code", status},
        {"description", description}
    };
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

// Methods that never consume rate-limit tokens: startup identity and the
// inbound long poll. Everything else is an outgoing call and is limited.
bool is_passthrough(const std::string& method) {
    return method == "getUpdates" || method == "getMe";
}

// Comma-separated list -> set of trimmed, non-empty strings.
std::set<std::string> parse_id_list(const std::string& raw) {
    std::set<std::string> out;
    std::stringstream ss(raw);
    std::string part;
    while (std::getline(ss, part, ',')) {
        const auto b = part.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        const auto e = part.find_last_not_of(" \t");
        out.insert(part.substr(b, e - b + 1));
    }
    return out;
}

// The chat a call is aimed at, as a string, or "" if the body has none we can read.
// python-telegram-bot sends form-encoded bodies (httplib parses those into req.params);
// JSON bodies are handled too. Anything unreadable yields "" (the call stays normal lane).
std::string chat_id_of(const httplib::Request& req) {
    if (req.has_param("chat_id")) return req.get_param_value("chat_id");
    if (req.get_header_value("Content-Type").find("json") != std::string::npos) {
        try {
            auto body = json::parse(req.body);
            if (body.is_object() && body.contains("chat_id")) {
                const auto& v = body["chat_id"];
                return v.is_string() ? v.get<std::string>() : v.dump();
            }
        } catch (const std::exception&) {
        }
    }
    return "";
}

const char* lane_name(Lane lane) { return lane == Lane::Low ? "low" : "normal"; }

}  // namespace

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Config: PORT (arg 1) and RATE_LIMIT (arg 2), everything else via env.
    int port = std::stoi(env_or("PORT", "8080"));
    if (argc > 1) port = std::stoi(argv[1]);

    double rate_limit = std::stod(env_or("RATE_LIMIT", "20"));
    if (argc > 2) rate_limit = std::stod(argv[2]);

    // Real bot token used for calls to Telegram. If unset, the token the client
    // put in the URL is used instead (handy for local testing).
    const std::string bot_token = env_or("BOT_TOKEN", "");

    // Shared secret. When set, the token slot of the URL must equal it. The bot
    // sends it as its "token", so python-telegram-bot needs no custom header.
    const std::string relay_secret = env_or("RELAY_SECRET", "");

    const std::string bind_addr = env_or("BIND_ADDR", "0.0.0.0");
    const auto max_wait = std::chrono::milliseconds(std::stol(env_or("MAX_WAIT_MS", "10000")));
    const size_t threads = std::stoul(env_or("THREADS", "64"));
    const bool dry_run = env_or("DRY_RUN", "") == "1";

    // Priority lanes. Calls aimed at LOW_PRIORITY_CHAT_IDS (e.g. a busy group chat
    // where spam can arrive) go in the low lane: served after every waiting normal call, paced by
    // their own bucket (Telegram allows about 20 messages/min to a group), and capped in how
    // many may wait so spam cannot take the whole thread pool. Unset means one lane (v1).
    const auto low_chat_ids = parse_id_list(env_or("LOW_PRIORITY_CHAT_IDS", ""));
    const double low_rate_per_min = std::stod(env_or("LOW_RATE_PER_MIN", "20"));
    const double low_burst = std::stod(env_or("LOW_BURST", "3"));
    const int low_max_waiting = std::stoi(env_or("LOW_MAX_WAITING", std::to_string(threads / 2)));

    auto limiter = std::make_shared<RateLimiter>(rate_limit);
    auto low_bucket = std::make_shared<RateLimiter>(low_rate_per_min / 60.0, low_burst);
    auto low_in_line = std::make_shared<std::atomic<int>>(0);  // low calls waiting for any token

    httplib::Server svr;
    // Every waiting request and every long poll holds a thread, so size the pool explicitly.
    svr.new_task_queue = [threads] { return new httplib::ThreadPool(threads); };

    svr.Get("/health", [limiter, low_bucket, low_in_line, low_chat_ids](const httplib::Request&,
                                                                        httplib::Response& res) {
        json response = {
            {"status", "ok"},
            {"rate_limit_per_second", limiter->rate()},
            {"available_tokens", limiter->available_tokens()},
            {"waiting", limiter->waiting()},
            {"paused", limiter->paused()},
            {"lanes", {
                {"normal", {{"waiting", limiter->waiting(Lane::Normal)}}},
                {"low", {
                    {"enabled", !low_chat_ids.empty()},
                    {"waiting", low_in_line->load()},
                    {"bucket_available_tokens", low_bucket->available_tokens()},
                    {"paused", low_bucket->paused()}
                }}
            }}
        };
        res.set_content(response.dump(2), "application/json");
    });

    svr.Post(R"(/bot([^/]+)/([A-Za-z]+))",
        [limiter, low_bucket, low_in_line, low_chat_ids, low_max_waiting, bot_token, relay_secret,
         max_wait, dry_run](const httplib::Request& req, httplib::Response& res) {
        const std::string token_slot = req.matches[1];
        const std::string method = req.matches[2];

        if (!relay_secret.empty() && token_slot != relay_secret) {
            set_tg_error(res, 401, "Unauthorized");
            return;
        }

        // The bot only ever sends JSON / form bodies (file_ids, never uploads).
        if (req.is_multipart_form_data()) {
            set_tg_error(res, 415, "multipart bodies are not supported by the relay");
            return;
        }

        const bool limited = !is_passthrough(method);
        const auto started = std::chrono::steady_clock::now();

        Lane lane = Lane::Normal;
        if (limited && !low_chat_ids.empty() && low_chat_ids.count(chat_id_of(req))) {
            lane = Lane::Low;
        }

        if (limited) {
            const auto deadline = started + max_wait;
            if (lane == Lane::Low) {
                // Shed immediately when too many low calls are already waiting.
                struct InLine {
                    std::atomic<int>& n;
                    explicit InLine(std::atomic<int>& n_) : n(n_) { n++; }
                    ~InLine() { n--; }
                } in_line(*low_in_line);
                if (low_in_line->load() > low_max_waiting) {
                    log_line([&](std::ostream& os) {
                        os << "[relay] " << method << " lane=low -> 503 (low lane full)";
                    });
                    set_tg_error(res, 503, "relay busy");
                    return;
                }
                // Own bucket first, without holding a global token, so a throttled low
                // lane never blocks normal calls.
                if (!low_bucket->acquire_by(deadline) || !limiter->acquire_by(deadline, Lane::Low)) {
                    log_line([&](std::ostream& os) {
                        os << "[relay] " << method << " lane=low -> 503 (relay busy)";
                    });
                    set_tg_error(res, 503, "relay busy");
                    return;
                }
            } else if (!limiter->acquire_by(deadline, Lane::Normal)) {
                log_line([&](std::ostream& os) { os << "[relay] " << method << " -> 503 (relay busy)"; });
                set_tg_error(res, 503, "relay busy");
                return;
            }
        }
        const auto waited_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();

        if (dry_run) {
            log_line([&](std::ostream& os) {
                os << "[relay] " << method << " lane=" << lane_name(lane) << " (dry run) waited="
                   << waited_ms << "ms";
            });
            res.status = 200;
            res.set_content(R"({"ok":true,"result":true})", "application/json");
            return;
        }

        const std::string upstream_token = bot_token.empty() ? token_slot : bot_token;
        const std::string path = "/bot" + upstream_token + "/" + method;

        auto cli = g_client_pool.acquire();
        // getUpdates is a long poll; everything else should answer quickly.
        cli->set_read_timeout(method == "getUpdates" ? 70 : 15, 0);

        std::string content_type = req.get_header_value("Content-Type");
        if (content_type.empty()) content_type = "application/json";

        const auto upstream_start = std::chrono::steady_clock::now();
        auto upstream = cli->Post(path, req.body, content_type);
        const auto upstream_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - upstream_start).count();
        if (!upstream) {
            log_line([&](std::ostream& os) {
                os << "[relay] " << method << " -> upstream error: " << httplib::to_string(upstream.error());
            });
            set_tg_error(res, 502, "relay could not reach Telegram");
            return;
        }

        // Telegram asked us to slow down: honor it for every caller.
        if (upstream->status == 429) {
            try {
                auto body = json::parse(upstream->body);
                double retry_after = body.at("parameters").at("retry_after").get<double>();
                // A throttled low-lane chat pauses only the low lane; anything else pauses all.
                if (lane == Lane::Low) {
                    low_bucket->pause_for(retry_after);
                } else {
                    limiter->pause_for(retry_after);
                }
                log_line([&](std::ostream& os) {
                    os << "[relay] 429 from Telegram (lane=" << lane_name(lane) << "), pausing "
                       << (lane == Lane::Low ? "the low lane" : "all calls") << " for "
                       << retry_after << "s";
                });
            } catch (const std::exception&) {
                // No usable retry_after; just pass the 429 through.
            }
        }

        if (method != "getUpdates") {
            log_line([&](std::ostream& os) {
                os << "[relay] " << method << " lane=" << lane_name(lane) << " -> " << upstream->status
                   << " waited=" << waited_ms << "ms"
                   << " upstream=" << upstream_ms << "ms"
                   << " total=" << (waited_ms + upstream_ms) << "ms";
            });
        }

        res.status = upstream->status;
        res.set_content(upstream->body, upstream->get_header_value("Content-Type").empty()
                                            ? "application/json"
                                            : upstream->get_header_value("Content-Type"));
    });

    std::cout << "===========================================" << std::endl;
    std::cout << " Telegram Relay Server (proxy) on port " << port << std::endl;
    std::cout << " Global rate limit:  " << rate_limit << " calls/sec" << std::endl;
    std::cout << " Max wait for token: " << max_wait.count() << " ms" << std::endl;
    std::cout << " Request threads:    " << threads << std::endl;
    if (low_chat_ids.empty()) {
        std::cout << " Low lane:           disabled (LOW_PRIORITY_CHAT_IDS unset)" << std::endl;
    } else {
        std::cout << " Low lane chats:     " << low_chat_ids.size() << " chat(s), "
                  << low_rate_per_min << "/min (burst " << low_burst << "), max waiting "
                  << low_max_waiting << std::endl;
    }
    std::cout << " Bot token:          " << (bot_token.empty() ? "(none — uses token from URL)" : "configured") << std::endl;
    std::cout << " Auth:               " << (relay_secret.empty() ? "disabled" : "secret in URL token slot") << std::endl;
    std::cout << " Dry run:            " << (dry_run ? "yes (no calls to Telegram)" : "no") << std::endl;
    std::cout << " Bind address:       " << bind_addr << std::endl;
    std::cout << " Endpoints:" << std::endl;
    std::cout << "   GET  /health" << std::endl;
    std::cout << "   POST /bot<token>/<method>" << std::endl;
    std::cout << " Press Ctrl+C to stop." << std::endl;
    std::cout << "===========================================" << std::endl;

    // Watcher: turns the signal flag into an orderly shutdown and releases
    // anyone blocked waiting for a token.
    std::thread watcher([&svr, limiter] {
        while (!g_stop) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        limiter->stop();
        svr.stop();
    });

    const bool listened = svr.listen(bind_addr, port);
    const bool stopped_by_signal = g_stop.load();
    g_stop = true;
    watcher.join();

    if (!listened && !stopped_by_signal) {
        std::cerr << "[FATAL] Failed to bind to " << bind_addr << ":" << port << std::endl;
        return 1;
    }
    return 0;
}
