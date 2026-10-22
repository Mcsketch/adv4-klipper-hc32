#!/usr/bin/env python3
# ad4-watch.py — poll M119 and print only the *changes*.
# Sit at the printer, press an endstop switch, watch the line flip.
#   usage: ad4-watch.py [seconds]   (default 60)
import asyncio, json, sys, time

from tornado.websocket import websocket_connect

URI = "ws://127.0.0.1:7125/websocket"


async def m119(ws):
    await ws.write_message(json.dumps({
        "jsonrpc": "2.0", "method": "printer.gcode.script",
        "params": {"script": "M119"}, "id": 1}))
    out = []
    while True:
        raw = await asyncio.wait_for(ws.read_message(), 5)
        if raw is None:
            raise ConnectionError("moonraker closed the socket")
        msg = json.loads(raw)
        if msg.get("method") == "notify_gcode_response":
            out.append(msg["params"][0])
        elif msg.get("id") == 1:
            return " ".join(out)


async def main(secs):
    last, ws = None, None
    end = time.time() + secs
    print("polling M119 every 0.5 s for %d s — actuate a switch now" % secs)
    while time.time() < end:
        try:
            if ws is None:
                ws = await websocket_connect(URI)
            cur = await m119(ws)
            if cur != last:
                print("%7.1fs  %s" % (secs - (end - time.time()), cur))
                last = cur
        except Exception as exc:
            # moonraker restarts (or klipper restarts) drop the socket; the
            # point of this tool is to survive them and keep watching.
            print("  (%s — reconnecting)" % exc)
            ws, last = None, None
        await asyncio.sleep(0.5)


if __name__ == "__main__":
    asyncio.run(main(int(sys.argv[1]) if len(sys.argv) > 1 else 60))
