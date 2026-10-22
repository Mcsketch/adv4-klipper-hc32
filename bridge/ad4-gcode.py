#!/usr/bin/env python3
# ad4-gcode.py — send gcode to klipper through moonraker and print the
# response (M119/M105/etc). The FIFO path swallows responses: klipper
# writes them back into the same fd it reads from, so nobody sees them.
#   usage: ad4-gcode.py "M119"
import asyncio, json, sys

from tornado.websocket import websocket_connect

URI = "ws://127.0.0.1:7125/websocket"


async def run(script, wait=6.0):
    ws = await websocket_connect(URI)
    await ws.write_message(json.dumps({
        "jsonrpc": "2.0", "method": "printer.gcode.script",
        "params": {"script": script}, "id": 1}))
    while True:
        raw = await asyncio.wait_for(ws.read_message(), wait)
        if raw is None:
            print("(connection closed)", file=sys.stderr)
            return
        msg = json.loads(raw)
        if msg.get("method") == "notify_gcode_response":
            p = msg["params"]
            print(p[0] if isinstance(p, list) else p["result"])
        elif msg.get("id") == 1:
            if msg.get("error"):
                print("ERROR:", msg["error"].get("message"))
            return


if __name__ == "__main__":
    asyncio.run(run(" ".join(sys.argv[1:]) or "M119"))
