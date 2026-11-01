# Telegram Relay Server

A small C++17 proxy that sits between your bot and `api.telegram.org` and rate-limits every
outgoing Bot API call, so a burst of activity can't get your bot throttled by Telegram.

```
your bot  ──HTTP──▶  relay  ──HTTPS──▶  api.telegram.org
          ◀────────        ◀────────
```

## How it works

- **Drop-in:** the relay accepts `POST /bot<token>/<method>`, the same URL shape as Telegram.
  Status code and body come back unchanged, so your client only needs a different base URL.
- **Rate limit:** one global token bucket (`RATE_LIMIT`, default 20 calls/s). A call waits for
  a token, is forwarded, and the caller gets Telegram's real response (`message_id`, errors).
- **Overload:** if a call can't get a token within `MAX_WAIT_MS` (default 10 s) it gets
  `503 relay busy`. Telegram's own `429` is passed back, and the relay pauses for `retry_after`.
- **Priority lanes (optional):** calls to chats listed in `LOW_PRIORITY_CHAT_IDS` (say, a noisy
  group) are served only when no normal call is waiting, have their own slower bucket, and are
  shed with an immediate `503` when too many are queued. Everything else is normal priority.
- **Never limited:** `getUpdates` and `getMe`.
- **Stateless:** nothing is queued on disk; a crash fails in-flight calls.

Not supported: multipart uploads (`415`; send a `file_id` or URL instead), GET requests, and
per-chat limits other than the low lane.

## Setup

Requires a C++17 compiler, CMake 3.20+ and OpenSSL.

```bash
cmake -B build -S . && cmake --build build
BOT_TOKEN=123:abc RELAY_SECRET=choose-a-secret ./build/telegram_relay_server
```

Or with Docker: `docker build -t telegram-relay . && docker run -p 8080:8080 -e BOT_TOKEN=... -e RELAY_SECRET=... telegram-relay`

| Variable | Default | Purpose |
| --- | --- | --- |
| `BOT_TOKEN` | – | Real token used to call Telegram. |
| `RELAY_SECRET` | – | If set, clients must put it in the URL token slot. |
| `PORT`, `BIND_ADDR` | `8080`, `0.0.0.0` | Listen address. Use `127.0.0.1` if the bot is on the same host. |
| `RATE_LIMIT` | `20` | Calls per second. |
| `MAX_WAIT_MS` | `10000` | Longest wait for a token before `503`. |
| `THREADS` | `64` | Request threads (each waiting call or long poll holds one). |
| `LOW_PRIORITY_CHAT_IDS` | – | Comma-separated chat ids for the low lane. |
| `LOW_RATE_PER_MIN`, `LOW_BURST`, `LOW_MAX_WAITING` | `20`, `3`, `THREADS/2` | Low-lane bucket and queue cap. |
| `DRY_RUN` | off | `1` skips the call to Telegram (for testing). |

## Point your client at it

Set the client's API base URL to the relay, and use `RELAY_SECRET` as the bot token. The real
token stays on the relay. Keep your client's request timeout above `MAX_WAIT_MS`.

```python
# python-telegram-bot
Application.builder().base_url("http://127.0.0.1:8080/bot").token(RELAY_SECRET).build()
```

Other libraries have an equivalent option (`baseApiUrl`, `apiRoot`, `APIEndpoint`, ...).
Check it works: `curl http://localhost:8080/health`.

## Load test results

`scripts/loadtest.py` (needs `httpx` and `ab`) runs the relay in `DRY_RUN` on loopback, so it
measures the relay itself, not Telegram. Release build, 8-core Mac.

| Scenario | Result |
| --- | --- |
| `RATE_LIMIT=20`, 400 concurrent calls | 398-400 served in ~19 s (20.9/s). No 1 s window after the initial burst exceeded 20. |
| 10 normal req/s, no other load | p50 3 ms, p99 5-9 ms |
| Same, plus a 40-connection low-lane flood | p50 38-49 ms, p99 39-50 ms (one token interval), 150/150 ok; ~42k excess low calls shed with `503` |
| Limiter not binding, `ab -c 32 -k` | ~100k req/s, p99 1 ms, 0 failures |

Known limit: the server uses one thread per open keep-alive connection. With more connections
than `THREADS` (a 200-connection flood against 64 threads), new connections wait for a thread
before the limiter sees them, and normal p50 rose to ~22 s. A bot with a small connection pool
is unaffected.

Unit tests: `ctest --test-dir build --output-on-failure`.

## License

MIT
