#!/usr/bin/env python3
"""Walk Z in small steps and report exactly where the Z endstop flips.

position_endstop has to sit ABOVE the switch's press point: klipper ends the
homing move AT position_endstop, so if the switch only closes a couple of mm
higher the move finishes untriggered and the screw grinds at the travel limit
("No trigger on stepper_z after full movement").

  python3 -I ad4-zendstop.py --down --from 160     # find the release edge
  python3 -I ad4-zendstop.py --up --from 150       # find the press edge

Reads and writes the toolhead Z through klipper's Unix socket, so it works
with the axis unhomed (needs [force_move]).
"""
import argparse
import json
import socket
import sys

DEFAULT_UDS = "/tmp/klippy_uds"


class KlippyRPC:
    # klipper frames each message with \x03 and answers gcode/script with a
    # {"params":{"response":...}} notification rather than a "result".
    def __init__(self, uds=DEFAULT_UDS, timeout=15.0):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.settimeout(timeout)
        self.s.connect(uds)
        self.buf = b""
        self.s.sendall(json.dumps(
            {"method": "gcode/subscribe_output", "params": {}}).encode()
            + b"\x03")

    def _read_msg(self):
        while b"\x03" not in self.buf:
            chunk = self.s.recv(4096)
            if not chunk:
                raise RuntimeError("klippy closed the connection")
            self.buf += chunk
        msg, self.buf = self.buf.split(b"\x03", 1)
        return json.loads(msg.decode())

    def script(self, cmd):
        self.s.sendall(json.dumps(
            {"method": "gcode/script", "params": {"script": cmd}}).encode()
            + b"\x03")
        while True:
            msg = self._read_msg()
            if "params" in msg and "response" in msg["params"]:
                return msg["params"]["response"]
            if "error" in msg:
                raise RuntimeError(msg["error"].get("message", str(msg)))

    def fire(self, cmd):
        # a script that prints nothing sends no reply at all, so do not wait
        self.s.sendall(json.dumps(
            {"method": "gcode/script", "params": {"script": cmd}}).encode()
            + b"\x03")

    def close(self):
        self.s.close()


def zstate(rpc):
    for tok in rpc.script("M119").split():
        if tok.startswith("stepper_z:"):
            return tok.split(":", 1)[1]
    raise RuntimeError("no stepper_z in M119 output")


def zpos(rpc):
    # M114 prints "X:.. Y:.. Z:.. E:.. Count ..."
    for tok in rpc.script("M114").split():
        if tok.startswith("Z:"):
            return float(tok[2:])
    raise RuntimeError("no Z in M114 output")


def main():
    ap = argparse.ArgumentParser()
    d = ap.add_mutually_exclusive_group(required=True)
    d.add_argument("--up", action="store_true", help="scan toward the switch")
    d.add_argument("--down", action="store_true", help="scan away from it")
    ap.add_argument("--from", dest="start", type=float, default=None,
                    help="start Z (default: current position)")
    ap.add_argument("--step", type=float, default=0.5)
    ap.add_argument("--limit", type=float, default=30.0,
                    help="max mm to travel before giving up")
    a = ap.parse_args()

    rpc = KlippyRPC()
    try:
        z = a.start if a.start is not None else zpos(rpc)
        want = "TRIGGERED" if a.up else "open"
        rpc.fire("SET_KINEMATIC_POSITION Z=%.3f" % z)
        rpc.fire("G90")
        print("z=%9.3f  stepper_z=%s" % (z, zstate(rpc)))
        sys.stdout.flush()
        moved = 0.0
        while moved < a.limit:
            z += a.step if a.up else -a.step
            moved += a.step
            rpc.fire("G1 Z%.3f F60" % z)
            # M400 blocks the gcode queue until the move physically finishes,
            # so the M119 below cannot read a state from mid-move.
            rpc.fire("M400")
            st = zstate(rpc)
            print("z=%9.3f  stepper_z=%s" % (z, st))
            sys.stdout.flush()
            if st == want:
                lo, hi = (z - a.step, z) if a.up else (z, z + a.step)
                print("\n%s edge: between Z=%.3f and Z=%.3f"
                      % ("press" if a.up else "release", lo, hi))
                return 0
        print("\nno flip within %.1f mm — the switch is not where you think"
              % a.limit)
        return 1
    finally:
        rpc.close()


if __name__ == "__main__":
    sys.exit(main())
