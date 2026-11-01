#!/usr/bin/env python3
"""Load test for the relay in DRY_RUN mode (no Telegram traffic).

Usage: ./scripts/loadtest.py   (build first; needs `pip install httpx` and `ab`)
Override the binary with RELAY_BIN=/path/to/telegram_relay_server.
"""
import asyncio, os, pathlib, subprocess, sys, tempfile, time
import httpx

BIN = os.environ.get("RELAY_BIN") or str(pathlib.Path(__file__).resolve().parent.parent / "build" / "telegram_relay_server")
PORT = 18080
SECRET = "s"
URL = f"http://127.0.0.1:{PORT}/bot{SECRET}/sendMessage"
LOW, NORMAL = "-100999", "12345"


def start(**env):
    e = dict(os.environ, DRY_RUN="1", RELAY_SECRET=SECRET, PORT=str(PORT), **{k: str(v) for k, v in env.items()})
    p = subprocess.Popen([BIN], env=e, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(50):
        try:
            httpx.get(f"http://127.0.0.1:{PORT}/health", timeout=0.5); return p
        except Exception: time.sleep(0.1)
    raise RuntimeError("server did not start")


def pct(xs, p):
    xs = sorted(xs); return xs[min(len(xs) - 1, int(len(xs) * p / 100))] if xs else float("nan")


def summary(name, lat_ms):
    return (f"{name}: n={len(lat_ms)} p50={pct(lat_ms,50):.1f}ms p95={pct(lat_ms,95):.1f}ms "
            f"p99={pct(lat_ms,99):.1f}ms max={max(lat_ms):.1f}ms") if lat_ms else f"{name}: n=0"


async def call(c, chat, out):
    t = time.perf_counter()
    try:
        r = await c.post(URL, data={"chat_id": chat, "text": "x"})
        code = r.status_code
    except Exception:
        code = -1
    out.append(((time.perf_counter() - t) * 1000, code, time.perf_counter()))


async def scenario_rate():
    """N concurrent clients, one burst: is the limit enforced and how fast does it drain?"""
    p = start(RATE_LIMIT=20, MAX_WAIT_MS=60000, THREADS=256)
    N = 400
    out = []
    async with httpx.AsyncClient(timeout=None, limits=httpx.Limits(max_connections=N)) as c:
        t0 = time.perf_counter()
        await asyncio.gather(*[call(c, NORMAL, out) for _ in range(N)])
        dur = time.perf_counter() - t0
    p.terminate()
    ok = [o for o in out if o[1] == 200]
    # completions per 1-second window (after the first 20-token burst)
    buckets = {}
    for _, _, ts in ok: buckets[int(ts - t0)] = buckets.get(int(ts - t0), 0) + 1
    worst = max(v for k, v in buckets.items() if k > 0)
    print(f"[rate] RATE_LIMIT=20/s, {N} concurrent calls: {len(ok)} ok in {dur:.2f}s "
          f"-> {len(ok)/dur:.2f} calls/s overall; max completions in any 1s window after burst={worst}")


async def normal_stream(c, rate, secs, out):
    tasks, t0, i = [], time.perf_counter(), 0
    while time.perf_counter() - t0 < secs:
        tasks.append(asyncio.create_task(call(c, NORMAL, out))); i += 1
        await asyncio.sleep(max(0, t0 + i / rate - time.perf_counter()))
    await asyncio.gather(*tasks)


async def low_flood(c, conc, stop, out):
    async def worker():
        while not stop.is_set(): await call(c, LOW, out)
    await asyncio.gather(*[worker() for _ in range(conc)])


async def scenario_priority(flood, conc=0):
    p = start(RATE_LIMIT=20, MAX_WAIT_MS=10000, THREADS=64, LOW_PRIORITY_CHAT_IDS=LOW,
              LOW_RATE_PER_MIN=60000, LOW_BURST=20)
    nout, lout, stop = [], [], asyncio.Event()
    async with httpx.AsyncClient(timeout=None, limits=httpx.Limits(max_connections=400)) as c:
        fp = subprocess.Popen([sys.executable, __file__, "flood", "17", str(conc)], stdout=subprocess.PIPE, text=True) if flood else None
        if flood: await asyncio.sleep(1)
        t0 = time.perf_counter()
        await normal_stream(c, 10, 15, nout)
        dur = time.perf_counter() - t0
    p.terminate()
    lat = [o[0] for o in nout if o[1] == 200]
    label = f"with {conc}-connection low-lane flood" if flood else "no flood (baseline)"
    print(f"[priority] {label}: normal 10 req/s for 15s, {len(lat)}/{len(nout)} ok")
    print("   " + summary("normal", lat))
    if flood:
        tot, served, shed = map(int, fp.communicate()[0].split())
        print(f"   low lane: {tot} attempts in ~17s: {served} served, {shed} shed (503)"
              f" -> {served/17:.1f} low/s served alongside {len(lat)/dur:.1f} normal/s")


async def flood_main(secs, conc):
    out, stop = [], asyncio.Event()
    async with httpx.AsyncClient(timeout=None, limits=httpx.Limits(max_connections=400)) as c:
        t = asyncio.create_task(low_flood(c, conc, stop, out))
        await asyncio.sleep(secs); stop.set(); await t
    print(len(out), sum(o[1] == 200 for o in out), sum(o[1] == 503 for o in out))


async def scenario_overhead():
    p = start(RATE_LIMIT=1000000, THREADS=64)
    body = tempfile.NamedTemporaryFile("w", suffix=".body", delete=False)
    body.write("chat_id=12345&text=x"); body.close()
    r = subprocess.run(["ab", "-q", "-n", "50000", "-c", "32", "-k", "-p", body.name, "-T",
                        "application/x-www-form-urlencoded", URL], capture_output=True, text=True).stdout
    p.terminate()
    keep = ("Requests per second", "Failed requests", "Non-2xx", "  50%", "  95%", "  99%", " 100%")
    print("[overhead] limiter not binding, ab -n 50000 -c 32 -k:")
    for l in r.splitlines():
        if l.strip().startswith(keep) or l.startswith(keep): print("   " + l.strip())


async def main():
    if sys.argv[1:2] == ["flood"]:
        return await flood_main(float(sys.argv[2]), int(sys.argv[3]))
    await scenario_rate()
    await scenario_priority(False)
    await scenario_priority(True, 40)
    await scenario_priority(True, 200)
    await scenario_overhead()

asyncio.run(main())
