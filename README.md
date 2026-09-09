# Relay Engine (C++ HTTP JSON Server)

A lightweight, high-performance C++ HTTP relay server built with [`cpp-httplib`](https://github.com/yhirose/cpp-httplib) and [`nlohmann/json`](https://github.com/nlohmann/json).

## Architecture

```
                       PRODUCER (Ingestion Layer)
  Telegram Webhooks /
     cURL Clients
          │
          │ HTTP POST /message (< 1ms response)
          ▼
   ┌────────────────────────────────────────────────────────────┐
   │                  cpp-httplib HTTP Server                   │
   │  1. Receive HTTP request                                   │
   │  2. Parse & validate JSON into `Job` struct                │
   │  3. Push Job onto queue: job_queue->push(job)              │
   │  4. Immediately send response: {"accepted": true}          │
   └─────────────────────────────┬──────────────────────────────┘
                                 │
                     BUFFER / SYNCHRONIZATION
                                 │
                                 ▼
   ┌────────────────────────────────────────────────────────────┐
   │               JobQueue (Thread-Safe Queue)                 │
   │  - std::queue<Job> protected by std::mutex                 │
   │  - std::condition_variable (zero CPU use when idle)        │
   │  - Holds jobs in memory awaiting available workers         │
   └─────────────────────────────┬──────────────────────────────┘
                                 │
                       CONSUMERS (Worker Layer)
                                 │
            ┌────────────────────┼────────────────────┐
            ▼                    ▼                    ▼
     ┌──────────────┐     ┌──────────────┐     ┌──────────────┐
     │  Worker #1   │     │  Worker #2   │     │  Worker #3   │
     │ (std::thread)│     │ (std::thread)│     │ (std::thread)│
     │              │     │              │     │              │
     │  pop(job)    │     │  pop(job)    │     │  pop(job)    │
     │  switch(type)│     │  switch(type)│     │  switch(type)│
     │  execute()   │     │  execute()   │     │  execute()   │
     └──────────────┘     └──────────────┘     └──────────────┘
```

1. **Ingestion Layer (`src/main.cpp`)**: High-throughput HTTP server that validates incoming JSON against the strongly-typed `Job` struct, pushes it to the queue, and acknowledges the request in `< 1ms`.
2. **Buffer Layer (`include/job_queue.hpp`)**: Thread-safe FIFO queue synchronized with `std::mutex` and `std::condition_variable` (idle workers sleep without burning CPU).
3. **Worker Pool (`include/worker_pool.hpp`)**: Configurable pool of background OS threads consuming jobs and executing actions (`SendText`, `SendPhoto`, `Delete`, etc.) in parallel.



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
# Default port (8080) with default 3 worker threads
./build/relay_server

# Specify custom port and custom worker count: ./build/relay_server [PORT] [WORKERS]
./build/relay_server 8080 5

# Or configure via environment variables
PORT=9000 WORKERS=8 ./build/relay_server
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
