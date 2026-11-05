#!/usr/bin/env python3
"""Run printer.cfg's TEST_SPEED through Moonraker and report skipped steps.

  ad4-speedtest.py SPEED ACCEL [ITERATIONS] [--url http://127.0.0.1:7125]

TEST_SPEED homes X and Y before and after the fast moves and prints
GET_POSITION each time. The X and Y step counts after both homings should
match to within the endstops' repeat (a fraction of a full step). Ctrl+C
sends an emergency stop. Roadmap Q4.
"""
import argparse, json, re, sys, urllib.parse, urllib.request

ap = argparse.ArgumentParser()
ap.add_argument("speed", type=float)
ap.add_argument("accel", type=float)
ap.add_argument("iterations", type=int, nargs="?", default=5)
ap.add_argument("--url", default="http://127.0.0.1:7125")
a = ap.parse_args()

def get(path):
    return json.load(urllib.request.urlopen(a.url + path, timeout=10))["result"]

def post(path, timeout=10):
    req = urllib.request.Request(a.url + path, method="POST")
    return urllib.request.urlopen(req, timeout=timeout).read()

st = get("/printer/objects/query?print_stats=state")["status"]["print_stats"]["state"]
if st not in ("standby", "complete", "cancelled"):
    sys.exit(f"printer is {st}; run only when idle")

cmd = f"TEST_SPEED SPEED={a.speed:g} ACCEL={a.accel:g} ITERATIONS={a.iterations}"
print(cmd, "(Ctrl+C = emergency stop)")
try:
    post("/printer/gcode/script?script=" + urllib.parse.quote(cmd), timeout=1800)
except KeyboardInterrupt:
    post("/printer/emergency_stop")
    sys.exit("emergency stop sent")

msgs = [m["message"] for m in get("/server/gcode_store?count=200")["gcode_store"]]
pos = [tuple(map(int, m.groups()))
       for m in (re.search(r"mcu: stepper_x:(-?\d+) stepper_y:(-?\d+)", s) for s in msgs) if m]
if len(pos) < 2:
    sys.exit("no GET_POSITION output found; see the console")
cfg = get("/printer/objects/query?configfile=settings")["status"]["configfile"]["settings"]
for i, ax in enumerate("xy"):
    sc = cfg["stepper_" + ax]
    usteps = sc.get("full_steps_per_rotation", 200) * sc["microsteps"]
    d = pos[-1][i] - pos[-2][i]
    full = d / sc["microsteps"]
    print(f"{ax.upper()}: {d:+d} microsteps = {full:+.2f} full steps = "
          f"{d * sc['rotation_distance'] / usteps:+.3f} mm  "
          f"{'OK' if abs(full) <= 1 else 'SKIPPED'}")
