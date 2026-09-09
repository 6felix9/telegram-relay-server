#!/usr/bin/env bash
set -e

PORT="${1:-8080}"
COUNT="${2:-30}"

echo "========================================================="
echo " Rate Limiter Test Script"
echo " Target:  http://localhost:${PORT}/"
echo " Sending: ${COUNT} rapid requests"
echo "========================================================="

# 1. Health check
HEALTH=$(curl -s "http://localhost:${PORT}/health" || echo "")
if [ -z "$HEALTH" ]; then
  echo "Error: Could not connect to server on port ${PORT}."
  echo "Make sure the server is running (e.g. ./build/telegram_relay_server ${PORT})"

  exit 1
fi

echo "Server status: ${HEALTH}"
echo ""
echo "Sending ${COUNT} requests concurrently..."

START_TIME=$(python3 -c 'import time; print(time.time())')

# Fire requests concurrently in background
for i in $(seq 1 "${COUNT}"); do
  curl -s -X POST "http://localhost:${PORT}/" \
    -H "Content-Type: application/json" \
    -d "{\"chat_id\": 100, \"message_id\": ${i}, \"type\": \"text\", \"text\": \"Rate limit test #${i}\"}" > /dev/null &
done

# Wait for all curl background processes to complete
wait

END_TIME=$(python3 -c 'import time; print(time.time())')
ELAPSED=$(python3 -c "print(f'{$END_TIME - $START_TIME:.3f}')")

echo "All ${COUNT} requests accepted by HTTP server in ${ELAPSED}s."
echo ""
echo "Check the server terminal log to observe the millisecond timestamps."
echo "Each outgoing call is paced by the global rate limiter across all workers."
echo "========================================================="
