#!/usr/bin/env bash
# Burst test: fire N concurrent sendMessage calls at the relay and check that the
# whole burst takes about (N - burst) / RATE_LIMIT seconds, i.e. the relay paced them.
#
# Run the relay without touching Telegram:
#   DRY_RUN=1 RATE_LIMIT=20 ./build/telegram_relay_server
# Usage: ./scripts/test_rate_limit.sh [PORT] [COUNT] [SECRET]
set -e

PORT="${1:-8080}"
COUNT="${2:-100}"
SECRET="${3:-test-secret}"   # must match RELAY_SECRET if the relay has one

echo "========================================================="
echo " Rate Limiter Test Script"
echo " Target:  http://localhost:${PORT}/bot<secret>/sendMessage"
echo " Sending: ${COUNT} concurrent requests"
echo "========================================================="

HEALTH=$(curl -s "http://localhost:${PORT}/health" || echo "")
if [ -z "$HEALTH" ]; then
  echo "Error: Could not connect to server on port ${PORT}."
  echo "Start it first, e.g.: DRY_RUN=1 ./build/telegram_relay_server ${PORT}"
  exit 1
fi
echo "Server status: ${HEALTH}"
echo ""

OUT_DIR=$(mktemp -d)
START_TIME=$(python3 -c 'import time; print(time.time())')

for i in $(seq 1 "${COUNT}"); do
  curl -s -o /dev/null -w "%{http_code}\n" -X POST \
    "http://localhost:${PORT}/bot${SECRET}/sendMessage" \
    -H "Content-Type: application/json" \
    -d "{\"chat_id\": 100, \"text\": \"Rate limit test #${i}\"}" > "${OUT_DIR}/${i}.code" &
done
wait

END_TIME=$(python3 -c 'import time; print(time.time())')
ELAPSED=$(python3 -c "print(f'{$END_TIME - $START_TIME:.3f}')")

echo "Finished ${COUNT} requests in ${ELAPSED}s. Status codes:"
cat "${OUT_DIR}"/*.code | sort | uniq -c
rm -rf "${OUT_DIR}"
echo ""
echo "Expected: about (${COUNT} - burst) / RATE_LIMIT seconds. Calls past MAX_WAIT_MS return 503."
echo "========================================================="
