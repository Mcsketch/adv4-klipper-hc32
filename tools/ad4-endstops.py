#!/usr/bin/env python3
"""Live endstop probe: talks klipper's JSON-RPC on its Unix socket.

M119 output only reaches the Fluidd console, so it never lands in klippy.log.
klipper answers `gcode/script` with the M119 line as a notification, which this
reads directly.

  python3 -I ad4-endstops.py                 # one shot
  python3 -I ad4-endstops.py -n 60 -i 0.2    # watch while you press a switch
  python3 -I ad4-endstops.py --mark          # print only on state change
"""
import argparse
import json
import socket
import sys
import time

DEFAULT_UDS = "/tmp/klippy_uds"


class KlippyRPC:
    # klipper frames each message with \x03 and answers gcode/script with a
    # {"params":{"response":...}} notification rather than a "result".
    def __init__(self, uds=DEFAULT_UDS, timeout=8.0):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.settimeout(timeout)
        self.s.connect(uds)
        self.buf = b""
        # klipper only pushes gcode responses to clients that subscribed;
        # the subscribe itself sends no reply, so nothing to read here.
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
        # klipper answers a script with a notification only if it printed
        # something, so a silent command must not be waited on.
        self.s.sendall(json.dumps(
            {"method": "gcode/script", "params": {"script": cmd}}).encode()
            + b"\x03")

    def close(self):
        self.s.close()


def parse(line):
    out = {}
    for tok in line.split():
        if ":" in tok:
            name, state = tok.split(":", 1)
            out[name] = state
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--uds", default=DEFAULT_UDS)
    ap.add_argument("-n", "--repeats", type=int, default=1)
    ap.add_argument("-i", "--interval", type=float, default=0.5)
    ap.add_argument("--mark", action="store_true",
                    help="only print when a state changes")
    a = ap.parse_args()

    rpc = KlippyRPC(a.uds)
    prev = None
    try:
        for i in range(a.repeats):
            st = parse(rpc.script("M119"))
            if not a.mark or st != prev:
                print("%9.3f  %s" % (time.monotonic() % 100000,
                                     "  ".join("%s=%s" % kv
                                               for kv in sorted(st.items()))))
                sys.stdout.flush()
            prev = st
            if i + 1 < a.repeats:
                time.sleep(a.interval)
    finally:
        rpc.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
