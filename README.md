# Relay Engine (C++ HTTP JSON Server)

A lightweight, high-performance C++ HTTP relay server built with [`cpp-httplib`](https://github.com/yhirose/cpp-httplib) and [`nlohmann/json`](https://github.com/nlohmann/json).

## Features

- Built with modern C++ (C++17).
- Zero external package dependencies needed: CMake automatically downloads and links `cpp-httplib` and `nlohmann/json` via `FetchContent`.
- **Job Data Model**:
  - `JobType`: `SendText`, `SendPhoto`, `SendVoice`, `SendVideoNote`, `SendSticker`, `Delete` (supports `snake_case` or `PascalCase` in JSON).
  - `Job`: `chat_id`, `message_id`, `reply_to_message_id`, `type`, `text`, `file_id`.
- Accepts Telegram event payloads, e.g.:
  ```json
  {
    "chat_id": 123,
    "message_id": 456,
    "reply_to_message_id": 100,
    "type": "text",
    "text": "Hello world"
  }
  ```
- Returns standard JSON response:
  ```json
  {
    "accepted": true
  }
  ```
- Handles input validation and malformed JSON errors gracefully (returns HTTP 400 with details).
- Graceful shutdown on `SIGINT` (Ctrl+C) and `SIGTERM`.
- Health check endpoint (`GET /health`).


## Prerequisites

- C++17 compatible compiler (`clang++` / `g++`)
- `cmake` (version 3.20+)

## Building the Project

```bash
# Configure and build
cmake -B build -S .
cmake --build build
```

The executable will be located at `build/relay_server`.

## Running the Server

```bash
# Default port (8080)
./build/relay_server

# Or specify a custom port
./build/relay_server 8088

# Or use the PORT environment variable
PORT=9000 ./build/relay_server
```

## Testing with cURL

### 1. Send Telegram Message Payload
```bash
curl -X POST http://localhost:8080/ \
  -H "Content-Type: application/json" \
  -d '{
    "chat_id": 123,
    "message_id": 456,
    "type": "text"
  }'
```

**Response (HTTP 200):**
```json
{
  "accepted": true
}
```

Endpoints `/`, `/relay`, and `/message` are all configured to accept this POST request.

### 2. Health Check
```bash
curl http://localhost:8080/health
```

**Response (HTTP 200):**
```json
{
  "status": "ok"
}
```
