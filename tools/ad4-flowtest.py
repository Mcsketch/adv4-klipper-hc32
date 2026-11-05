#!/usr/bin/env python3
"""Extrude-in-air flow test through Moonraker. Roadmap Q4.

  ad4-flowtest.py prep [TEMP]          home if needed, nozzle up, heat (default 240)
  ad4-flowtest.py run RATE [LENGTH]    extrude LENGTH mm (default 100) at RATE mm3/s
  ad4-flowtest.py calc LENGTH MARK GAP fraction actually fed

Before each run, mark the filament MARK mm (120) from the PTFE tube's spool
end. After it, measure the mark to the tube end (GAP). Fed = MARK - GAP; the
highest RATE that still feeds about 95 % is the hot end's limit at TEMP.
run also reports the lowest nozzle temperature and highest heater power
seen during the extrusion: a heater at 100 % means the heater is the limit.
Ctrl+C sends an emergency stop.
"""
import json, sys, threading, time, urllib.parse, urllib.request

URL = "http://127.0.0.1:7125"
FIL_AREA = 3.14159265 * 1.75 ** 2 / 4

def get(path):
    return json.load(urllib.request.urlopen(URL + path, timeout=10))["result"]

def gcode(script, timeout=600):
    req = urllib.request.Request(URL + "/printer/gcode/script?script=" + urllib.parse.quote(script), method="POST")
    urllib.request.urlopen(req, timeout=timeout).read()

def status(q):
    return get("/printer/objects/query?" + q)["status"]

def idle():
    st = status("print_stats=state")["print_stats"]["state"]
    if st not in ("standby", "complete", "cancelled"):
        sys.exit(f"printer is {st}; run only when idle")

a = sys.argv[1:]
if not a or a[0] not in ("prep", "run", "calc"):
    sys.exit(__doc__)
try:
    if a[0] == "prep":
        idle()
        temp = float(a[1]) if len(a) > 1 else 240
        if status("toolhead=homed_axes")["toolhead"]["homed_axes"] != "xyz":
            gcode("G28")
        gcode(f"G90\nG1 Z50 F300\nG1 X110 Y100 F3000\nM109 S{temp:g}")
        print(f"nozzle at {temp:g}, Z 50. Mark the filament, then: run RATE")
    elif a[0] == "run":
        idle()
        rate = float(a[1])
        length = float(a[2]) if len(a) > 2 else 100
        e = status("extruder=temperature,target")["extruder"]
        if e["target"] < 170 or e["temperature"] < e["target"] - 3:
            sys.exit(f"nozzle {e['temperature']:.0f}/{e['target']:.0f}: run prep first")
        f = rate / FIL_AREA
        print(f"{length:g} mm at {rate:g} mm3/s = {f:.2f} mm/s filament, {length / f:.0f} s")
        lo, hi, done = e["temperature"], 0.0, threading.Event()
        def watch():
            global lo, hi
            while not done.is_set():
                s = status("extruder=temperature,power")["extruder"]
                lo, hi = min(lo, s["temperature"]), max(hi, s["power"])
                time.sleep(0.5)
        t = threading.Thread(target=watch, daemon=True)
        t.start()
        t0 = time.time()
        gcode(f"M83\nG1 E{length:g} F{f * 60:.1f}\nM400")
        done.set(); t.join()
        print(f"took {time.time() - t0:.0f} s, nozzle min {lo:.1f}, heater max {hi * 100:.0f} %. Measure the gap.")
    else:
        length, mark, gap = map(float, a[1:4])
        fed = mark - gap
        print(f"fed {fed:g} of {length:g} mm = {100 * fed / length:.1f} %")
except KeyboardInterrupt:
    urllib.request.urlopen(urllib.request.Request(URL + "/printer/emergency_stop", method="POST"), timeout=10)
    sys.exit("emergency stop sent")
