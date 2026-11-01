# Telegram Relay Server

A small C++17 proxy that sits between a Telegram bot and `api.telegram.org` and rate-limits
every outgoing Bot API call. The bot calls the relay instead of Telegram; the relay waits for
a rate-limit token, forwards the call, and returns Telegram's real response.

```
your bot (any client)  ──HTTP──▶  relay (C++)  ──HTTPS──▶  api.telegram.org
                     ◀────────                ◀────────
```

It is a **transparent, synchronous proxy**:

- **Transparent:** same URL shape as Telegram (`POST /bot<token>/<method>`). Status code and
  body come back unchanged, so clients like python-telegram-bot work with only a base-URL change.
- **Synchronous:** the HTTP request stays open while the call waits for its turn, so the caller
  gets the real result (`message_id`, errors) instead of "accepted".

## Behavior

- **Rate limit:** one global token bucket, `RATE_LIMIT` calls per second (default 20).
- **Priority lanes:** calls to a chat in `LOW_PRIORITY_CHAT_IDS` (e.g. a busy group chat) are
  *low*; everything else is *normal*. Normal calls are always served first. The low lane has its
  own bucket (`LOW_RATE_PER_MIN`, default 20) and at most `LOW_MAX_WAITING` waiting calls; extra
  low calls get an immediate `503`. The lane is read from the `chat_id` in the request body;
  unreadable means normal. Unset `LOW_PRIORITY_CHAT_IDS` means a single lane. Per-chat limits
  for other chats are not implemented.
- **Pass-through, never limited:** `getUpdates` (long poll) and `getMe`.
- **Overload:** a call that cannot get a token within `MAX_WAIT_MS` (default 10000) gets
  `503 {"ok":false,"error_code":503,"description":"relay busy"}`.
- **Telegram 429:** passed back unchanged. The relay pauses the lane that got it (low-lane 429 pauses only the low lane; a normal call's 429 pauses everything) for `retry_after`.
  The relay never retries a call itself.
- **Auth:** if `RELAY_SECRET` is set, the token slot of the URL must equal it (else `401`).
  The bot sends the secret where its token would go, so PTB needs no custom header.
- **Token:** the relay calls Telegram with `BOT_TOKEN`. If that is unset, it uses whatever
  token the client put in the URL (handy for local testing).
- **State:** none. A crash fails in-flight calls; nothing is queued or lost silently.
- **Limits:** multipart bodies are rejected (`415`); the bot only sends `file_id`s.

## Build and run

Prerequisites: C++17 compiler, `cmake` 3.20+, OpenSSL (on macOS `openssl@3` via Homebrew).

```bash
cmake -B build -S .
cmake --build build

BOT_TOKEN=123:abc RELAY_SECRET=hunter2 BIND_ADDR=127.0.0.1 ./build/telegram_relay_server
```

| Variable | Default | Purpose |
| --- | --- | --- |
| `PORT`, `RATE_LIMIT` | `8080`, `20` | Also positional args 1 and 2. |
| `BOT_TOKEN` | *(none)* | Real token used to call Telegram. |
| `RELAY_SECRET` | *(none)* | Required value of the URL token slot. Unset means no auth. |
| `MAX_WAIT_MS` | `10000` | Longest wait for a rate-limit token before `503`. |
| `THREADS` | `64` | Request threads. Every waiting call and long poll holds one. |
| `BIND_ADDR` | `0.0.0.0` | Use `127.0.0.1` when the bot is on the same host. |
| `LOW_PRIORITY_CHAT_IDS` | *(none)* | Comma-separated chat ids routed to the low lane. |
| `LOW_RATE_PER_MIN`, `LOW_BURST` | `20`, `3` | Low-lane bucket rate and burst. |
| `LOW_MAX_WAITING` | `THREADS / 2` | Low calls allowed to wait; more get `503` at once. |
| `DRY_RUN` | *(off)* | `1` skips the call to Telegram and returns `{"ok":true,"result":true}`. |

## Using it from a bot

Point the client's base URL at the relay and send `RELAY_SECRET` where the bot token would go.
With python-telegram-bot:

```python
Application.builder().base_url("http://127.0.0.1:8080/bot").token(RELAY_SECRET)
```

Keep the client's request timeouts above the relay's `MAX_WAIT_MS`, or the client gives up on a
call the relay may still send.

## Testing

Burst test without touching Telegram:

```bash
DRY_RUN=1 RATE_LIMIT=20 RELAY_SECRET=test-secret ./build/telegram_relay_server
./scripts/test_rate_limit.sh 8080 100 test-secret   # 100 calls take about 4s at 20/s
```

Unit tests and the lane burst test:

```bash
cmake --build build && ctest --test-dir build --output-on-failure
DRY_RUN=1 RATE_LIMIT=5 LOW_PRIORITY_CHAT_IDS=-100999 LOW_RATE_PER_MIN=1200 RELAY_SECRET=test-secret MAX_WAIT_MS=8000 ./build/telegram_relay_server
./scripts/test_priority_lanes.py 8080 test-secret 100 10   # normal calls finish ahead of the low queue
```

## Load test results

`scripts/loadtest.py` (needs `httpx` and `ab`) runs the relay in `DRY_RUN` on loopback, so
it measures the relay itself, not Telegram. Release build, 8-core Mac:

| Scenario | Result |
| --- | --- |
| `RATE_LIMIT=20`, 400 concurrent calls | 398-400 of 400 served in ~19s (20.9/s). No 1s window after the initial burst had more than 20 completions. |
| 10 normal req/s, no other load | p50 2.9 ms, p99 9.1 ms, 150/150 ok |
| Same, plus a 40-connection low-lane flood | p50 38-49 ms, p99 39-50 ms (one token interval), 150/150 ok. Low calls got the leftover ~11/s, ~42k excess calls shed with an immediate `503`. |
| Limiter not binding, `ab -c 32 -k` | ~100k req/s, p99 1 ms, 0 failures (no Telegram call) |

Known limit: the HTTP server uses one worker thread per open keep-alive connection. With more
connections than `THREADS` (e.g. a 200-connection flood against 64 threads), new connections wait
for a thread before the limiter sees them, and priority lanes cannot help (normal p50 rose to ~22s).
A bot client with a small connection pool is unaffected.

Health check: `curl http://localhost:8080/health` returns the rate, available tokens and
the number of calls currently waiting.

Real call: `curl -X POST http://localhost:8080/bot<secret>/getMe`.
