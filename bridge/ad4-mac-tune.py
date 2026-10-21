#!/usr/bin/env python3
# =============================================================
# ad4-mac-tune.py — G-code tuning client for the AD4 Klipper host.
#
# Talks to klippy's gcode PTY (/dev/adv4_gcode, created by
# ad4-mac-start.sh via klippy -I). No Moonraker needed.
#
# Usage:
#   ad4-mac-tune.py             # interactive menu
#   ad4-mac-tune.py "G28"       # fire one gcode line + print reply
#   ad4-mac-tune.py --raw       # raw paste mode (type gcode directly)
#
# The MCU must be connected in klippy (see ~/klipper-hc32/klippy.log)
# or the commands will queue/time out.
# =============================================================
import os, sys, termios, select, tty

# gcode PTY location: ad4-mac-start.sh creates it under ~/adv4/links/gcode
# (macOS /dev is root-only, so symlinks live in the home dir). Allow an env
# override; resolve the real device via realpath at open time.
_DEFAULT_GCODE = os.path.expanduser("~/adv4/links/gcode")
GCODE_PTY = os.environ.get("AD4_GCODE_PTY", _DEFAULT_GCODE)

def open_pty(path, timeout=2.0):
    import serial
    # open the resolved real device (symlink target); PTY baud is cosmetic.
    real = os.path.realpath(path)
    s = serial.Serial(real, 115200, timeout=timeout, write_timeout=1)
    s.reset_input_buffer()
    return s

def pump_read(s, dur=2.0, live=False):
    """Read whatever klippy prints for dur seconds."""
    import time
    end = time.time() + dur
    while time.time() < end:
        n = s.in_waiting
        if n:
            data = s.read(n)
            if live:
                sys.stdout.buffer.write(data); sys.stdout.flush()
            yield data
        else:
            import time as _t; _t.sleep(0.05)

def run_line(s, line, wait=2.5, live=True):
    if live:
        print(f"\n\033[1;36m> {line}\033[0m")
    s.write((line + "\n").encode())
    s.flush()
    for chunk in pump_read(s, dur=wait, live=live):
        sys.stdout.buffer.write(chunk); sys.stdout.flush()
    print()

# canned tuning commands for the AD4 first-contact tests
MENU = [
    ("1", "FIRMWARE_RESTART", "re-init MCU (safety)"),
    ("2", "M115", "version / handshake"),
    ("3", "G28", "home all axes"),
    ("4", "G30", "Z probe (bed mesh if enabled)"),
    ("5", "G1 X0 Y0 F3000", "move to origin"),
    ("6", "G1 Z5 F3000", "lift Z 5mm"),
    ("7", "M105", "report temps"),
    ("8", "M104 S0", "extruder temp 0"),
    ("9", "M140 S0", "bed temp 0"),
    ("0", "M112", "EMERGENCY STOP (careful)"),
    ("s", "status", "show klippy.log tail"),
    ("q", "quit", ""),
]

def menu(s):
    import subprocess
    print("=== AD4 Klipper tuning (gcode PTY: %s) ===" % GCODE_PTY)
    print("  (Enter to re-show; number for canned; gcode> to type raw)\n")
    for k, cmd, desc in MENU:
        print(f"  {k:>2}  {cmd:<22} {desc}")
    while True:
        sys.stdout.write("\n\033[1m[ad4] >\033[0m ")
        sys.stdout.flush()
        line = sys.stdin.readline().strip()
        if not line:
            for k, cmd, desc in MENU:
                print(f"  {k:>2}  {cmd:<22} {desc}")
            continue
        if line in ("q", "quit", "exit"):
            print("bye"); break
        if line == "s":
            subprocess.run(["tail", "-n", "15", "/Users/admin/klipper-hc32/klippy.log"])
            continue
        if line.startswith("gcode>") or line not in dict((m[0], 1) for m in MENU):
            run_line(s, line, wait=4.0)
            continue
        for k, cmd, desc in MENU:
            if k == line:
                if line == "0":
                    print("\033[1;31mEMERGENCY STOP — this halts the printer.\033[0m "
                          "Type 'yes' to run M112: ", end=""); sys.stdout.flush()
                    if input().strip().lower() != "yes":
                        print("cancelled"); break
                run_line(s, cmd, wait=6.0)
                break

def main():
    if not os.path.exists(GCODE_PTY):
        print(f"ERROR: gcode PTY {GCODE_PTY} not found.")
        print("  Is klippy running? (ad4-mac-start.sh). Tail ~/klipper-hc32/klippy.log")
        sys.exit(1)
    s = open_pty(GCODE_PTY)
    print(f"connected to {GCODE_PTY}")
    try:
        if len(sys.argv) > 1 and sys.argv[1] == "--raw":
            import time
            print("raw mode (Ctrl-D to end). typing gcode...")
            while True:
                try:
                    l = input("gcode> ")
                except EOFError:
                    break
                if l.strip():
                    run_line(s, l.strip(), wait=3.0)
        elif len(sys.argv) > 1:
            run_line(s, " ".join(sys.argv[1:]), wait=4.0)
        else:
            menu(s)
    finally:
        s.close()

if __name__ == "__main__":
    main()
