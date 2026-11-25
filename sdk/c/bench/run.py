#!/usr/bin/env python3
"""C vs Lua benchmark driver (README.md): runs the bench apps over the badge's
serial console and prints the table.

    python3 run.py push --port P lua                    # copy bench-lua to the card
    python3 run.py push --port P c --wasm app.wasm      # copy bench-c (app.wasm: interpreted)
    python3 run.py run --port P bench-lua --out r.jsonl [workload ...]
    python3 run.py run --port P bench-c --out r.jsonl --label "C native"
    python3 run.py run --port P bench-c --out r.jsonl --ramp mandel   # grow until stopped
    python3 run.py table r.jsonl                        # the tables, from every run saved

`run` launches the app once per workload, picks it from the app's menu with
`key` commands, waits for its DONE line (or the error screen), closes it and
reads `lua mem`: whether a C app ran native (app.aot) or interpreted, and what
the probe callback counted against its limit. With --fw it also runs
`lua benchfw <workload>`, which only a test build of Solana OS has (the same C
compiled into the firmware). Each result is a JSON line in --out; the raw
console goes to --log. Needs pyserial (the system python3, as tools/badge.py).
"""
import argparse, base64, hashlib, json, os, re, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
WORKLOADS = ["mandel", "mandelf", "mandeld", "sieve", "sha", "shaapi", "fill", "copy",
             "draw", "draw80", "labels", "list", "game", "alloc", "stack", "ui"]
LIMIT_WL = {"alloc", "stack", "ui"}
CAPS = {"lua": 200000, "wasm": 2000000, "native_us": 200000, "host": 200000}


class Console:
    def __init__(self, port, log):
        import serial
        self.s = serial.Serial()
        self.s.port, self.s.baudrate, self.s.timeout = port, 115200, 0.05
        self.s.dtr = self.s.rts = False
        self.s.open()
        self.log = open(log, "a") if log else None
        self.buf = ""
        head = self.read_until(["ESP-ROM", "rst:"], 1.5)
        if head is not None:
            self.read_until(["console ready"], 30)    # opening the port reset the board
        time.sleep(1.5)
        self.drain()

    def _read(self):
        d = self.s.read(8192)
        if d:
            t = d.decode("utf-8", "replace")
            self.buf += t
            if self.log:
                self.log.write(t)
                self.log.flush()
        return bool(d)

    def drain(self):
        while self._read():
            pass

    def read_until(self, markers, timeout, start=0):
        """Waits for any marker in what arrived since `start`; returns the text or None."""
        end = time.time() + timeout
        while time.time() < end:
            for m in markers:
                if m in self.buf[start:]:
                    return self.buf[start:]
            self._read()
        return None

    def cmd(self, c, timeout=30):
        mark = len(self.buf)
        self.s.write((c + "\r\n").encode())
        self.read_until(["sol> "], timeout, mark)
        time.sleep(0.15)
        self.drain()
        return self.buf[mark:]

    def send_file(self, app, name, path):
        data = open(path, "rb").read()
        sha = hashlib.sha256(data).hexdigest()
        b64 = base64.b64encode(data)
        mark = len(self.buf)
        self.s.write(f"lua recv {app} {name} {len(data)} {sha}\r\n".encode())
        pos = 0
        while True:
            got = self.read_until(["recv: ready ", "recv: more ", "recv: ok", "recv: error", "usage:"], 30, mark)
            if got is None:
                return False
            m = re.search(r"recv: (ready|more) (\d+)\s", got)
            if m:
                mark += got.index(m.group(0)) + len(m.group(0))
                n = int(m.group(2))
                self.s.write(b64[pos:pos + n])
                pos += n
                continue
            ok = "recv: ok" in got
            self.read_until(["sol> "], 5, mark)
            print(f"  {app}/{name}: {len(data)} bytes {'ok' if ok else 'FAILED'}")
            return ok


def app_lines(text, app):
    """The app's own console lines, from its "[<id>] " tag on (a prompt may come first)."""
    tag = f"[{app}] "
    return [l[l.index(tag):].strip() for l in text.splitlines() if tag in l]


def parse_lines(r):
    """CHK, BENCH, PROBE and the last FIT line of a result, as dicts."""
    for l in r.get("lines", []):
        l = l[l.index("["):] if "[" in l else l
        kind = l.split()[1] if len(l.split()) > 1 else ""
        if kind in ("CHK", "BENCH", "PROBE"):
            r[kind.lower()] = fields(l)
        elif kind == "FIT":
            r["fit_last"] = fields(l)
    return r


def fields(line):
    return dict(kv.split("=", 1) for kv in line.split()[2:] if "=" in kv)


def parse_mem(text):
    r = {}
    for key, rx in [("lua", r"most instructions in one callback (\d+)"), ("host", r"most host steps .*? in one callback (\d+)"),
                    ("wasm", r"most wasm instructions in one callback (\d+)"),
                    ("native_us", r"most native time in one callback (\d+) us"), ("heap_peak", r"heap peak (\d+) bytes"),
                    ("error", r"last error: (.*)")]:
        m = re.search(rx, text)
        if m:
            r[key] = m.group(1).strip() if key == "error" else int(m.group(1))
    if "app.aot (native)" in text:
        r["form"] = "native"
    elif "app.wasm (interpreted)" in text:
        r["form"] = "interpreted"
    return r


def run_one(con, app, wl, ramp, timeout):
    con.cmd("open home")
    mark = len(con.buf)
    t0 = time.time()
    con.s.write(f"lua run {app}\r\n".encode())
    if con.read_until(["READY "], 15, mark) is None:
        return {"wl": wl, "error": "no READY (is the app on the card?)"}
    launch_ms = (time.time() - t0) * 1000
    con.read_until(["sol> "], 3, mark)
    text = con.buf[mark:]
    menu = next((l for l in app_lines(text, app) if " MENU " in l), "")
    ready = fields(next(l for l in app_lines(text, app) if " READY " in l))
    names = menu.split(" MENU ", 1)[1].split(",") if menu else WORKLOADS
    res = {"wl": wl, "launch_ms": round(launch_ms), "ready_ms": int(ready.get("ms", -1))}
    if wl == "mandel":  # once per run: the badge's memory with the app open, idle in its menu
        res["heap_open"] = con.cmd("heap")
        res["mem_open"] = con.cmd("lua mem")
    for _ in range(names.index(wl)):
        con.cmd("key down")
    mark = len(con.buf)
    con.cmd("key right" if ramp else "key select")
    got = con.read_until(["DONE wl=", f"[lua:{app}] "], timeout, mark)
    time.sleep(0.5)
    con.drain()
    out = con.buf[mark:]
    res["lines"] = app_lines(out, app)
    err = re.search(rf"\[lua:{re.escape(app)}\] (.*)", out)
    if err:
        res["stop"] = err.group(1).strip()
    if got is None:
        res["timeout"] = True
    con.cmd("key back")
    con.cmd("open home")
    res["mem"] = parse_mem(con.cmd("lua mem"))
    return parse_lines(res)


def run_fw(con, wl, os_):
    out = con.cmd(f"lua benchfw {wl}{' os' if os_ else ''}", timeout=60)
    if "usage:" in out:
        return None
    return parse_lines({"wl": wl, "lines": app_lines(out, "benchfw")})


def cmd_push(a):
    con = Console(a.port, a.log)
    if a.which == "lua":
        ok = con.send_file("bench-lua", "app.json", os.path.join(HERE, "lua/app.json")) and \
            con.send_file("bench-lua", "main.lua", os.path.join(HERE, "lua/main.lua"))
    else:
        if not a.wasm:
            sys.exit("push c needs --wasm (build it: tools/cc/badge-cc sdk/c/bench/c -o app.wasm)")
        ok = con.send_file("bench-c", "app.json", os.path.join(HERE, "c/app.json")) and \
            con.send_file("bench-c", "app.wasm", a.wasm)
    sys.exit(0 if ok else 1)


def cmd_run(a):
    con = Console(a.port, a.log)
    outf = open(a.out, "a")
    wls = a.workloads or WORKLOADS
    base = {"app": a.app, "label": a.label, "when": time.strftime("%Y-%m-%d %H:%M:%S")}
    if a.settle:
        time.sleep(a.settle)  # after a reset: let the boot's Wi-Fi attempt (30 s) end
    if a.wifi_off:
        con.cmd("wifi off")  # for this boot only: the saved network stays
        time.sleep(2)
    base["wifi"] = "off" if "(off)" in con.cmd("wifi status") else "joined or on"
    info = con.cmd("info")
    base["os"] = (re.search(r"Solana OS (\S+)", info) or [None, "?"])[1]
    base["heap_launcher"] = con.cmd("heap")
    for wl in wls:
        if a.fw or a.fwos:
            if wl in LIMIT_WL:
                continue
            r = run_fw(con, wl, a.fwos)
            if r is None:
                print(f"{wl}: no `lua benchfw` on this badge (a test build has it)")
                continue
            r.update(base, label=a.label or ("C in the firmware (-Os)" if a.fwos else "C in the firmware (-O2)"))
        else:
            r = run_one(con, a.app, wl, wl in (a.ramp or []), a.timeout)
            r.update(base)
            if not r.get("label"):
                form = r.get("mem", {}).get("form")
                r["label"] = "Lua" if a.app == "bench-lua" else f"C {form or '?'}"
        outf.write(json.dumps(r) + "\n")
        outf.flush()
        short = {k: r.get(k) for k in ("chk", "bench", "probe", "stop", "fit_last") if r.get(k)}
        print(f"{r['label']:24} {wl:8} {json.dumps(short)}")


# --- the tables ---------------------------------------------------------------------

def per_unit_us(r):
    b = r.get("bench")
    if not b or not int(b["units"]):
        return None
    return int(b["us"]) / int(b["units"])  # drawing: the blit included


def fit_per_callback(r):
    """Average units one callback can hold, and the limit that sets it. `lua mem`
    gives the most each limit's counter counted in one callback, the app the
    longest callback's work time (max_us): their ratio is what the counter
    counts per microsecond of this work; the run gives an average unit's time."""
    p, m, u = r.get("probe"), r.get("mem", {}), per_unit_us(r)
    longest = int(p.get("max_us", p.get("us", 0))) if p else 0
    if not longest or not u:
        return None, None
    best = None
    for key, cap in (("lua", CAPS["lua"]), ("wasm", CAPS["wasm"]), ("native_us", CAPS["native_us"]), ("host", CAPS["host"])):
        used = m.get(key)
        if not used or (key == "host" and used < 10000):  # under 10 000: the app's own printing, not the probe
            continue
        fit = int(cap * longest / (used * u))
        if best is None or fit < best[0]:
            best = (fit, key)
    if r.get("app") == "bench-c":  # a C app's callback also stops at 3 s of wall-clock time
        fit = int(3000000 / u)
        if best is None or fit < best[0]:
            best = (fit, "3 s")
    return best if best else (None, None)


def cmd_table(a):
    rows = [parse_lines(json.loads(l)) for f in a.files for l in open(f) if l.strip()]
    labels = []
    for r in rows:
        if r.get("label") not in labels:
            labels.append(r["label"])
    by = {(r["label"], r["wl"]): r for r in rows}   # the last run of each wins
    print("| Workload | " + " | ".join(labels) + " |")
    print("|---|" + "---|" * len(labels))
    for wl in WORKLOADS:
        if wl in LIMIT_WL:
            continue
        cells = []
        for lb in labels:
            r = by.get((lb, wl))
            u = per_unit_us(r) if r else None
            if u is None:
                cells.append("" if not r else (r.get("stop") or "-"))
                continue
            chk = r.get("chk", {}).get("chk", "")
            fit, lim = fit_per_callback(r)
            b = r["bench"]
            frames = int(b.get("frames", 0))
            fps = f", {frames * 1000 / int(b['wall']):.1f} fps" if frames else ""
            cells.append(f"{u:.1f} us/unit{fps}" + (f", {fit}/cb ({lim})" if fit else "") + (f", chk {chk}" if chk else ""))
        print(f"| {wl} | " + " | ".join(cells) + " |")
    print()
    for r in rows:
        if r["wl"] in LIMIT_WL or r.get("stop"):
            last = [l for l in r.get("lines", []) if " ALLOC " in l or " STACK " in l or " UI " in l][-1:]
            print(f"{r['label']:24} {r['wl']:8} {' '.join(last)}  stop: {r.get('stop', '-')}")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    for name in ("push", "run"):
        q = sub.add_parser(name)
        q.add_argument("--port", required=True)
        q.add_argument("--log", default=None, help="append the raw console here")
        if name == "push":
            q.add_argument("which", choices=["lua", "c"])
            q.add_argument("--wasm")
        else:
            q.add_argument("app", choices=["bench-lua", "bench-c"])
            q.add_argument("workloads", nargs="*")
            q.add_argument("--out", required=True)
            q.add_argument("--label")
            q.add_argument("--ramp", nargs="*", help="these workloads grow until the badge stops them")
            q.add_argument("--timeout", type=float, default=240)
            q.add_argument("--settle", type=float, default=0, help="seconds to wait first (35 after a reset)")
            q.add_argument("--wifi-off", action="store_true", help="turn Wi-Fi off first (until the next restart)")
            q.add_argument("--fw", action="store_true", help="lua benchfw (test builds only), -O2")
            q.add_argument("--fwos", action="store_true", help="lua benchfw ... os (test builds only), -Os")
    t = sub.add_parser("table")
    t.add_argument("files", nargs="+")
    a = p.parse_args()
    {"push": cmd_push, "run": cmd_run, "table": cmd_table}[a.cmd](a)


if __name__ == "__main__":
    main()
