#!/usr/bin/env python3
# ad4-fluidd-proxy.py — raw-socket reverse proxy.
#  :8081  serves Fluidd static (~/moonraker/fluidd)
#  /api /websocket /server /machine  ->  Moonraker 127.0.0.1:7125
# Raw socket (not BaseHTTPRequestHandler) so we can pipe the WebSocket
# upgrade and arbitrary bodies transparently.
import os, socket, socketserver, threading

PORT = 8081
MR_HOST, MR_PORT = "127.0.0.1", 7125
FLUIDD = os.path.expanduser("~/moonraker/fluidd")
# First path segment -> Moonraker API; anything else is a Fluidd SPA route.
API_SEGMENTS = {b"api", b"websocket", b"server", b"machine", b"printer",
                b"gcode_script", b"object", b"job_queue", b"history",
                b"update", b"announcement", b"webcam", b"database",
                b"spoolman", b"power", b"template", b"extensions"}
CT = {b".html":b"text/html",b".js":b"text/javascript",b".css":b"text/css",
      b".json":b"application/json",b".png":b"image/png",b".svg":b"image/svg+xml",
      b".ico":b"image/x-icon",b".woff":b"font/woff",b".woff2":b"font/woff2",
      b".wasm":b"application/wasm",b".map":b"application/json",b".txt":b"text/plain"}

def recv_headers(sock):
    """Read request line + headers (until blank line). Return first-line + all bytes."""
    buf = b""
    while not buf.endswith(b"\r\n\r\n") and not buf.endswith(b"\n\n"):
        c = sock.recv(1)
        if not c:
            break
        buf += c
        if len(buf) > 65536:
            break
    first = buf.split(b"\n", 1)[0].strip()
    return first, buf

def pipe(a, b):
    try:
        while True:
            d = a.recv(65536)
            if not d:
                break
            b.sendall(d)
    except OSError:
        pass

def serve_static(sock, path):
    if path in (b"", b"/"):
        path = b"/index.html"
    fp = os.path.normpath(os.path.join(FLUIDD, path.lstrip(b"/").decode()))
    if not fp.startswith(FLUIDD) or not os.path.isfile(fp):
        fp = os.path.join(FLUIDD, "index.html")  # SPA fallback
    try:
        data = open(fp, "rb").read()
    except OSError:
        sock.sendall(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
        return
    ext = os.path.splitext(fp)[1].lower().encode()
    sock.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: " + CT.get(ext, b"application/octet-stream") +
                 b"\r\nContent-Length: " + str(len(data)).encode() +
                 b"\r\nConnection: close\r\n\r\n" + data)

def proxy(sock, headbytes):
    try:
        up = socket.create_connection((MR_HOST, MR_PORT), timeout=5)
    except OSError:
        sock.sendall(b"HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
        return
    up.sendall(headbytes)
    t1 = threading.Thread(target=pipe, args=(sock, up), daemon=True)
    t2 = threading.Thread(target=pipe, args=(up, sock), daemon=True)
    t1.start(); t2.start()
    t1.join(); t2.join()
    try:
        up.close()
    except OSError:
        pass

class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        sock = self.request
        try:
            first, head = recv_headers(sock)
            path = first.split(b" ", 2)[1].split(b"?", 1)[0] if b" " in first else b""
            if path.lstrip(b"/").split(b"/", 1)[0] in API_SEGMENTS:
                proxy(sock, head)
            else:
                serve_static(sock, path)
        except Exception:
            pass
        finally:
            try:
                sock.close()
            except OSError:
                pass

class Srv(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True
    daemon_threads = True

if __name__ == "__main__":
    print(f"Fluidd proxy: http://0.0.0.0:{PORT}  static={FLUIDD}  -> moonraker {MR_HOST}:{MR_PORT}", flush=True)
    Srv(("0.0.0.0", PORT), Handler).serve_forever()
