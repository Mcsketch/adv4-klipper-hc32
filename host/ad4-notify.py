#!/usr/bin/env python3
"""Phone notifications for print jobs (roadmap M4).

Polls Moonraker's print_stats every few seconds and runs a command on each
job event:

    CMD TITLE MESSAGE PRIORITY [IMAGE_URL]

PRIORITY is min, default, high or urgent. IMAGE_URL (the camera snapshot) is
passed on finish, pause and error when --image is set. The command is
anything that sends a message: on Caleb's hub it is the lab's notify.sh
(Home Assistant companion app).

Events: started (min, with the finish time), paused (high), finished
(default), cancelled (min), error (urgent). Nothing is sent for the state
found at start-up, and nothing when Klipper loses the printer while idle
(switching the printer off is normal).

    ad4-notify.py --cmd /usr/local/bin/notify.sh \\
        --image http://192.168.1.10:8095/webcam/snapshot
"""
import argparse
import json
import subprocess
import sys
import time
import urllib.parse
import urllib.request


def get(base, path):
    with urllib.request.urlopen(base + path, timeout=5) as r:
        return json.load(r)["result"]


def hm(sec):
    m = int(sec + 30) // 60
    return f"{m // 60}:{m % 60:02d}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--moonraker", default="http://127.0.0.1:7125")
    ap.add_argument("--cmd", required=True)
    ap.add_argument("--image", default="")
    ap.add_argument("--name", default="Adventurer 4")
    ap.add_argument("--interval", type=float, default=5)
    a = ap.parse_args()

    def send(title, msg, prio, image=False):
        argv = [a.cmd, f"{a.name}: {title}", msg, prio]
        if image and a.image:
            argv.append(a.image)
        print(" | ".join(argv[1:4]), flush=True)
        subprocess.run(argv, timeout=30)

    last = None
    while True:
        try:
            st = get(a.moonraker, "/printer/objects/query?print_stats"
                     "&display_status=progress&webhooks")["status"]
        except Exception:
            time.sleep(a.interval)
            continue
        ps = st.get("print_stats", {})
        state = ps.get("state", "")
        wh = st.get("webhooks", {})
        # a shutdown mid-job (MCU lost, heater fault) may leave print_stats as is
        if wh.get("state") != "ready" and last in ("printing", "paused"):
            state = "error"
        job = ps.get("filename", "").rsplit("/", 1)[-1].removesuffix(".gcode")
        if last is not None and state != last:
            if state == "printing" and last != "paused":
                msg = job
                try:
                    q = urllib.parse.quote(ps.get("filename", ""))
                    est = get(a.moonraker, f"/server/files/metadata?filename={q}")["estimated_time"]
                    msg += time.strftime(", done about %H:%M", time.localtime(time.time() + est))
                except Exception:
                    pass
                send("printing", msg, "min")
            elif state == "paused":
                pct = st.get("display_status", {}).get("progress", 0) * 100
                send("paused", f"{job} at {pct:.0f} %", "high", True)
            elif state == "complete":
                send("done", f"{job} in {hm(ps.get('total_duration', 0))}", "default", True)
            elif state == "cancelled":
                send("cancelled", job, "min")
            elif state == "error" and last in ("printing", "paused"):
                why = ps.get("message") or wh.get("state_message", "")
                send("print failed", f"{job}: {why.strip()}"[:300], "urgent", True)
        last = state
        time.sleep(a.interval)


if __name__ == "__main__":
    sys.exit(main())
