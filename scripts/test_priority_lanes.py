#!/usr/bin/env python3
"""Mixed-burst test for priority lanes (run the relay in DRY_RUN, no Telegram needed):

  DRY_RUN=1 RATE_LIMIT=5 LOW_PRIORITY_CHAT_IDS=-100999 LOW_RATE_PER_MIN=1200 \
      RELAY_SECRET=test-secret MAX_WAIT_MS=8000 ./build/telegram_relay_server

Usage: ./scripts/test_priority_lanes.py [PORT] [SECRET] [LOW_COUNT] [NORMAL_COUNT]

Fires LOW_COUNT low-priority-chat calls (form-encoded, as python-telegram-bot sends them),
then NORMAL_COUNT private-chat calls. The normal calls must all finish long before the low
queue drains.
"""
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

port = sys.argv[1] if len(sys.argv) > 1 else "8080"
secret = sys.argv[2] if len(sys.argv) > 2 else "test-secret"
low_count = int(sys.argv[3]) if len(sys.argv) > 3 else 100
normal_count = int(sys.argv[4]) if len(sys.argv) > 4 else 10
LOW_CHAT, NORMAL_CHAT = "-100999", "12345"

url = f"http://localhost:{port}/bot{secret}/sendMessage"
results = {"low": [], "normal": []}
lock = threading.Lock()


def call(kind, chat_id):
    body = urllib.parse.urlencode({"chat_id": chat_id, "text": "x"}).encode()
    start = time.time()
    try:
        status = urllib.request.urlopen(urllib.request.Request(url, body), timeout=30).status
    except urllib.error.HTTPError as e:
        status = e.code
    with lock:
        results[kind].append((time.time() - start, status))


threads = [threading.Thread(target=call, args=("low", LOW_CHAT)) for _ in range(low_count)]
for t in threads:
    t.start()
time.sleep(0.3)  # let the low calls queue up first
t0 = time.time()
normals = [threading.Thread(target=call, args=("normal", NORMAL_CHAT)) for _ in range(normal_count)]
for t in normals:
    t.start()
for t in normals:
    t.join()
normal_done = time.time() - t0
low_done_by_then = sum(1 for r in results["low"] if r[1] == 200)  # served, not shed
for t in threads:
    t.join()

max_normal = max(r[0] for r in results["normal"])
ok_normal = sum(1 for r in results["normal"] if r[1] == 200)
print(f"normal: {ok_normal}/{normal_count} ok, all done in {normal_done:.2f}s, slowest {max_normal:.2f}s")
print(f"low:    {len(results['low'])} finished, {low_done_by_then} served (200) before the normals did")
statuses = {}
for _, s in results["low"]:
    statuses[s] = statuses.get(s, 0) + 1
print(f"low statuses: {statuses}")

# Normal calls must be served ahead of the queued low calls: at the global rate they need
# about normal_count / RATE_LIMIT seconds, not the time to drain the low queue. Low calls over
# LOW_MAX_WAITING are shed with 503 at once, which keeps request threads free for normal calls.
failed = ok_normal != normal_count or max_normal > 5.0 or low_done_by_then > low_count // 2
print("FAIL" if failed else "PASS")
sys.exit(1 if failed else 0)
