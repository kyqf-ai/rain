#!/usr/bin/env python3
"""End-to-end test: fake device on a pty -> ./rain -> SQLite -> web API."""
import os, sys, time, json, signal, shutil, sqlite3, subprocess, urllib.request, urllib.error, re
from datetime import datetime, timedelta

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import devsim

TMP = "/tmp/rain_e2e"
fails = checks = 0
def check(cond, what):
    global fails, checks
    checks += 1
    if not cond:
        fails += 1; print("FAIL:", what)

def start_dev(*args):
    p = subprocess.Popen([sys.executable, os.path.join(ROOT, "tools", "devsim.py"), *args],
                         stdout=subprocess.PIPE, text=True)
    return p, p.stdout.readline().strip()

def write_ini(name, pty_path, port=18080, machine="SRY1C-2211-00001", extra=""):
    d = os.path.join(TMP, name); os.makedirs(d, exist_ok=True)
    ini = os.path.join(d, "rain.ini")
    open(ini, "w").write(f"""[serial]
port={pty_path}
[device]
machine={machine}
[collect]
poll_sec=5
backfill_hours=30
timeout_ms=800
reconnect_sec=2
{extra}
[web]
port={port}
[paths]
db={d}/rain.db
logdir={d}/logs
""")
    return ini, os.path.join(d, "rain.db")

def run_once(ini):
    return subprocess.run([os.path.join(ROOT, "rain"), "-c", ini, "once"], capture_output=True, text=True, timeout=120)

def get(port, path):
    try:
        r = urllib.request.urlopen(f"http://127.0.0.1:{port}{path}", timeout=5)
        return r.status, r.headers, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.headers, e.read()

def verify_hours(db, label, min_rows=28):
    con = sqlite3.connect(db)
    now = datetime.now() + timedelta(seconds=11)
    cur = now.replace(minute=0, second=0, microsecond=0)
    rows = {r[0]: r for r in con.execute("SELECT hour,total,temp10,final FROM rain_hour")}
    check(len(rows) >= min_rows, f"{label}: expected >= {min_rows} hours stored, got {len(rows)}")
    bad = 0
    for k, (hour, total, temp, final) in rows.items():
        h = datetime.strptime(hour, "%Y-%m-%d %H:%M:%S")
        want = sum(devsim.hour_minutes(h))
        if h < cur:
            if total != want or final != 1 or temp is None: bad += 1
        elif h == cur:
            if not (0 <= total <= want) or final != 0 or temp is not None: bad += 1
    check(bad == 0, f"{label}: {bad} hour rows differ from the device's data")
    mins = con.execute("SELECT COUNT(*) FROM rain_min").fetchone()[0]
    want_min = sum(1 for hour in rows for v in devsim.hour_minutes(datetime.strptime(hour, "%Y-%m-%d %H:%M:%S")) if v)
    check(mins <= want_min and mins >= want_min - 8, f"{label}: minute rows {mins} vs expected ~{want_min}")
    con.close()
    return rows

shutil.rmtree(TMP, ignore_errors=True); os.makedirs(TMP)
procs = []
try:
    # 1. little-endian device with 5% corrupted replies
    dev, pty1 = start_dev("--bad-crc", "0.05"); procs.append(dev)
    ini, db = write_ini("le", pty1, extra="max_fetch=40")
    r = run_once(ini)
    check(r.returncode == 0, f"once exit code {r.returncode}\n{r.stdout}{r.stderr}")
    rows = verify_hours(db, "LE")
    n1 = len(rows)
    r = run_once(ini)                                   # second run must be idempotent
    check(r.returncode == 0, "second once failed")
    check(len(verify_hours(db, "LE again")) in (n1, n1 + 1), "second run changed row count unexpectedly")
    con = sqlite3.connect(db)
    st = con.execute("SELECT state,v_static,v_sun,v_alarm,v_hang,temp_c,clock_off FROM device_state").fetchone()
    check(st and st[0] == "online" and st[1:6] == (7014, 9000, 5900, 5800, 22), f"device_state {st}")
    check(st and 5 <= st[6] <= 17, f"clock offset {st[6] if st else None} (device simulated +11 s)")
    con.close()

    # 2. long-running mode + web API
    run = subprocess.Popen([os.path.join(ROOT, "rain"), "-c", ini, "run"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    procs.append(run)
    time.sleep(7)
    s, h, body = get(18080, "/")
    check(s == 200 and b"<canvas" in body and "text/html" in h["Content-Type"], "index page")
    s, h, body = get(18080, "/api/day")
    d = json.loads(body)
    check(s == 200 and len(d["hours"]) == 24 and d["unit"] == 100, "day json shape")
    today = datetime.now().strftime("%Y-%m-%d")
    bad = [x for x in d["hours"] if x["s"] == 2 and abs(x["v"] - sum(devsim.hour_minutes(datetime.strptime(today, "%Y-%m-%d") + timedelta(hours=x["h"]))) / 100) > 0.0051]
    check(not bad, f"today's final hours differ from the device: {bad}")
    check(any(x["s"] for x in d["hours"]), "today has data")
    yday = (datetime.now() - timedelta(days=1)).strftime("%Y-%m-%d")
    s, h, body = get(18080, f"/api/day?date={yday}")
    dy = json.loads(body)
    want_y = sum(sum(devsim.hour_minutes(datetime.strptime(yday, "%Y-%m-%d") + timedelta(hours=hh))) for hh in range(24)) / 100
    have_hours = [x for x in dy["hours"] if x["s"]]
    check(s == 200 and all(x["s"] == 2 for x in have_hours), "yesterday hours final")
    exp_have = sum(sum(devsim.hour_minutes(datetime.strptime(yday, "%Y-%m-%d") + timedelta(hours=x["h"]))) for x in have_hours) / 100
    check(abs(dy["total"] - exp_have) < 0.011, f"yesterday total {dy['total']} vs {exp_have}")
    rain_h = next((x for x in dy["hours"] if x["s"] and x["v"] > 0), None)
    if rain_h:
        s, h, body = get(18080, f"/api/minutes?hour={yday}%20{rain_h['h']:02d}")
        m = json.loads(body)
        check(s == 200 and len(m["min"]) == 60 and abs(m["total"] - rain_h["v"]) < 0.011 and sum(1 for v in m["min"] if v) >= 3, "minute detail")
    s, h, body = get(18080, f"/api/range?from={yday}&to={today}&by=day")
    rg = json.loads(body)
    check(s == 200 and abs(rg["summary"]["hours_with_data"] - min(31, 24 + datetime.now().hour + 1)) <= 1 and rg["summary"]["total"] >= 0, f"range json {rg['summary']}")
    s, h, body = get(18080, f"/export.csv?from={yday}&to={today}&by=day")
    check(s == 200 and "text/csv" in h["Content-Type"] and body.startswith(b"time,rain_mm"), "csv export")
    s, h, body = get(18080, "/api/status")
    stj = json.loads(body)
    check(s == 200 and stj["state"] == "online" and stj["v_static"] == 7014 and stj["machine"] == "SRY1C-2211-00001", "status json")
    check(get(18080, "/api/day?date=2026-13-01")[0] == 400, "bad date -> 400")
    check(get(18080, "/api/range?from=2026-01-01&to=2026-12-31&by=hour")[0] == 400, "huge hourly range -> 400")
    check(get(18080, "/api/range?from=2026-01-01&to=2026-01-02&by=evil")[0] == 400, "bad grouping -> 400")
    check(get(18080, "/api/day?date=2026-01-01%27%20OR%201=1")[0] == 400, "injection attempt rejected")
    check(get(18080, "/nope")[0] == 404, "unknown path -> 404")
    check(get(18080, "/api/ping")[0] == 200, "ping")
    if shutil.which("node"):
        u = subprocess.run(["node", os.path.join(ROOT, "tests", "ui_smoke.js"), "http://127.0.0.1:18080"], capture_output=True, text=True, timeout=60)
        check(u.returncode == 0 and "ui smoke: ok" in u.stdout, "UI smoke test:\n" + u.stdout + u.stderr)
    else:
        print("note: node not found, UI smoke test skipped")
    run.send_signal(signal.SIGINT)
    try: out, _ = run.communicate(timeout=10)
    except subprocess.TimeoutExpired:
        run.kill(); out = ""; check(False, "rain did not stop on SIGINT")
    check(run.returncode == 0, f"clean shutdown, exit code {run.returncode}")
    con = sqlite3.connect(db)
    check(con.execute("SELECT state FROM device_state").fetchone()[0] == "stopped", "state 'stopped' after shutdown")
    con.close()
    dev.kill()

    # 2b. catch-up is spread over several cycles when max_fetch is small
    dev, ptyc = start_dev(); procs.append(dev)
    inic, dbc = write_ini("catchup", ptyc, port=18084, extra="max_fetch=10")
    counts = []
    for _ in range(4):
        check(run_once(inic).returncode == 0, "catch-up cycle failed")
        counts.append(len(sqlite3.connect(dbc).execute("SELECT hour FROM rain_hour").fetchall()))
    check(counts[0] <= 12 and counts[0] < counts[1] < counts[2] and counts[-1] >= 30, f"catch-up progression {counts}")
    verify_hours(dbc, "catch-up")
    dev.kill()

    # 2c. the control path the GUI uses (thread start/stop/restart, status, poke, failure states)
    dev, ptyk = start_dev(); procs.append(dev)
    os.makedirs(os.path.join(TMP, "ctl"), exist_ok=True)
    r = subprocess.run([os.path.join(ROOT, "tests", "test_ctl"), ptyk, os.path.join(TMP, "ctl", "rain.db")], capture_output=True, text=True, timeout=120)
    check(r.returncode == 0, "test_ctl:\n" + r.stdout + r.stderr)
    print("   test_ctl:", r.stdout.strip().splitlines()[-1] if r.stdout.strip() else r.stderr[:200])
    dev.kill()

    # 2d. firmware rollover (seen in the field log 2026-11-08): a request for one hour is
    #     answered with the NEXT hour's record. The answer must be stored under its own
    #     hour, the requested hour recorded as a data gap (no row), and it must not be
    #     re-requested by later cycles of the same process.
    gap = (datetime.now() - timedelta(days=2)).replace(hour=23, minute=0, second=0, microsecond=0)
    dev, ptyr = start_dev("--rollover", gap.strftime("%Y-%m-%d %H")); procs.append(dev)
    ini, db = write_ini("rollover", ptyr, port=18085, extra="backfill_hours=96\nmax_fetch=200")
    logf = os.path.join(TMP, "rollover", "logs", "rain-%s.log" % datetime.now().strftime("%Y%m%d"))
    r = run_once(ini)
    check(r.returncode == 0, f"rollover once failed\n{r.stdout}{r.stderr}")
    log = open(logf).read()
    gk = gap.strftime("%Y-%m-%d %H:%M:%S")
    nk = (gap + timedelta(hours=1)).strftime("%Y-%m-%d %H:%M:%S")
    con = sqlite3.connect(db)
    check(con.execute("SELECT COUNT(*) FROM rain_hour WHERE hour=?", (gk,)).fetchone()[0] == 0,
          "rollover hour must stay a data gap (no row)")
    row = con.execute("SELECT total, final FROM rain_hour WHERE hour=?", (nk,)).fetchone()
    want = sum(devsim.hour_minutes(gap + timedelta(hours=1)))
    check(row and row[0] == want and row[1] == 1, f"rollover answer stored as {nk}: {row}, want ({want}, 1)")
    con.close()
    check(log.count("device rollover") == 1, f"one rollover warning per sweep, got {log.count('device rollover')}")
    r = run_once(ini)                                   # incremental cycle: gap must not be re-requested
    log = open(logf).read()
    check(r.returncode == 0 and log.count("device rollover") == 1,
          f"incremental cycle re-requested the gap ({log.count('device rollover')} warnings)")
    run = subprocess.Popen([os.path.join(ROOT, "rain"), "-c", ini, "run"],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    procs.append(run)
    time.sleep(9)                                       # startup sweep re-probes old gaps once
    run.send_signal(signal.SIGINT)
    try: run.communicate(timeout=10)
    except subprocess.TimeoutExpired:
        run.kill(); check(False, "rollover run did not stop on SIGINT")
    log = open(logf).read()
    check(log.count("device rollover") == 2,
          f"startup sweep re-probes the gap exactly once, got {log.count('device rollover')}")
    con = sqlite3.connect(db)
    check(con.execute("SELECT COUNT(*) FROM rain_hour WHERE hour=?", (gk,)).fetchone()[0] == 0,
          "gap hour still absent after re-probe")
    row = con.execute("SELECT total, final FROM rain_hour WHERE hour=?", (nk,)).fetchone()
    check(row and row[0] == want and row[1] == 1, f"next-hour row unchanged after re-probe: {row}")
    con.close()
    dev.kill()

    # 2e. provisional dedup (field log: 51 identical writes for one hour in 4 min at
    #     poll=5 s): the current hour must only be written/logged when its value changes
    dev, ptyp = start_dev("--rain-every-hour"); procs.append(dev)
    ini, db = write_ini("prov", ptyp, port=18086)
    run = subprocess.Popen([os.path.join(ROOT, "rain"), "-c", ini, "run"],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    procs.append(run)
    time.sleep(17)
    run.send_signal(signal.SIGINT)
    try: run.communicate(timeout=10)
    except subprocess.TimeoutExpired:
        run.kill(); check(False, "prov run did not stop on SIGINT")
    logp = os.path.join(TMP, "prov", "logs", "rain-%s.log" % datetime.now().strftime("%Y%m%d"))
    log = open(logp).read()
    prov = re.findall(r"hour (\d{4}-\d{2}-\d{2} \d{2}):\d{2}:\d{2}: +([\d.]+) mm \(provisional\)", log)
    check(len(prov) >= 1, "current hour was logged at least once")
    check(len(prov) == len(set(prov)), f"provisional log lines must be unique (hour, value): {prov}")
    con = sqlite3.connect(db)
    curh = datetime.now().replace(minute=0, second=0, microsecond=0).strftime("%Y-%m-%d %H:%M:%S")
    row = con.execute("SELECT total, final FROM rain_hour WHERE hour=?", (curh,)).fetchone()
    check(row and row[0] > 0 and row[1] == 0, f"current hour stored as provisional: {row}")
    con.close()
    dev.kill()

    # 3. big-endian device: auto byte order detection must give the same totals
    dev, pty2 = start_dev("--endian", "be"); procs.append(dev)
    ini, db = write_ini("be", pty2, port=18081, extra="max_fetch=40")
    r = run_once(ini)
    check(r.returncode == 0, f"BE once failed\n{r.stdout}{r.stderr}")
    verify_hours(db, "BE-auto")
    dev.kill()

    # 4. wrong machine id is refused and nothing is stored
    dev, pty3 = start_dev(); procs.append(dev)
    ini, db = write_ini("wrong", pty3, port=18082, machine="OTHER-0000")
    r = run_once(ini)
    check(r.returncode == 2, f"wrong machine should exit 2, got {r.returncode}")
    con = sqlite3.connect(db)
    check(con.execute("SELECT COUNT(*) FROM rain_hour").fetchone()[0] == 0, "no data stored for wrong machine")
    check(con.execute("SELECT state FROM device_state").fetchone()[0] == "machine_mismatch", "mismatch state recorded")
    con.close(); dev.kill()

    # 5. device that was never there: port error is reported, not a crash
    ini, db = write_ini("noport", "/dev/does-not-exist", port=18083)
    r = run_once(ini)
    check(r.returncode == 2, f"missing port should exit 2, got {r.returncode}")

    # 6. report command
    ini, db = write_ini("le", pty1)
    r = subprocess.run([os.path.join(ROOT, "rain"), "-c", ini, "report", yday], capture_output=True, text=True)
    check(r.returncode == 0 and "Total" in r.stdout and "00:00" in r.stdout, "report command")
finally:
    for p in procs:
        if p.poll() is None: p.kill()
print(f"e2e: {checks} checks, {fails} failed")
sys.exit(1 if fails else 0)
