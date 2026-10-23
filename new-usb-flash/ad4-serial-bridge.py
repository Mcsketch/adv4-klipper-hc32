#!/usr/bin/env python3
"""
ad4-serial-bridge  —  bridge the Adventurer 4 MCU (on the SoC's /dev/ttyS1)
to a TCP/UDP port so a Klipper host (Raspberry Pi) on the other end of the
Ethernet link can drive the MCU with NO soldering.

Why this exists
---------------
The SoC is already wired to the MCU (factory wiring, uart0 == /dev/ttyS1).
Klipper's MCU layer wants a local serial device, but the RPi is the host.
So:
    RPi (Klipper)  <==ethernet==>  SoC (this bridge)  <==UART==>  MCU
and the RPi presents the remote MCU as a local PTY (socat on the RPi).
This is the classic ser2net-over-LAN Klipper topology. Zero soldering.

Two transports (pick one):
    --tcp  (default)  a single client at a time; use for Klipper.
    --udp             connectionless; lower per-packet overhead, but no
                      ordering guarantee — only if you measure TCP as too slow.

Protocol note
-------------
Klipper's MCU protocol is a raw byte stream over the serial port; the
host sends "ADC 0\n"-style command lines and the MCU replies with
"<" ... ">" frames. The MCU serial BAUD is 250000 (Klipper default).
The bridge must be byte-transparent (no newline translation, no echo).
We set the tty to raw, 8N1, 250000 and pump bytes both directions with
minimal buffering.

Latency
-------
A direct point-to-point 100/1000 MbE cable between the RPi and the SoC
gives sub-millisecond latency, comfortably within Klipper's real-time
budget (the MCU protocol is timing-sensitive: the host expects the MCU's
clock-sync reply promptly). Keep the SoC otherwise idle while printing.

Run (on the SoC, root, after stopping the vendor stack — see stop-vendor.sh):
    python3 ad4-serial-bridge.py --tcp 0.0.0.0:5000 --baud 250000
    python3 ad4-serial-bridge.py --udp 0.0.0.0:5000 --baud 250000
"""
import argparse, os, select, sys, termios, tty, fcntl, struct, socket

B1 = {300:0, 1200:1, 2400:2, 4800:3, 9600:4, 19200:5, 38400:6, 57600:7,
      115200:8, 230400:9, 460800:10, 921600:11, 250000:13, 500000:14,
      1000000:15, 2000000:16}

def setup_uart(port, baud):
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    tty.setraw(fd)
    # 8N1
    attrs[2] = (attrs[2] & ~termios.CSIZE) | termios.CS8
    attrs[2] &= ~(termios.CSTOPB | termios.PARENB | termios.CRTSCTS)
    attrs[2] |= termios.CLOCAL | termios.CREAD
    attrs[0] &= ~(termios.ICRNL | termios.IGNCR | termios.IXON | termios.INLCR)
    attrs[0] = 0
    attrs[3] &= ~termios.ECHO
    if baud in B1:
        attrs[4] = attrs[5] = B1[baud]
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    # flush any junk in both directions before hand-off
    try:
        fcntl.ioctl(fd, getattr(termios, "TCFLSH", termios.TCIOFLSH), termios.TCIOFLSH)
    except Exception:
        pass
    return fd

def pump(fd, sock, timeout=30.0, verbose=False):
    """Bidirectional byte pump until the peer closes or the tty errors."""
    running = True
    last = os.times()
    try:
        while running:
            r, _, x = select.select([fd, sock], [], [], 1.0)
            if fd in x or sock in x:
                raise OSError("serial/sock exception")
            if fd in r:
                try:
                    data = os.read(fd, 65536)
                except OSError as e:
                    if e.errno in (errno_EAGAIN,): continue
                    raise
                if data:
                    sock.sendall(data)
                    if verbose: sys.stderr.write(f"[tx {len(data)}]\n"); sys.stderr.flush()
            if sock in r:
                try:
                    data = sock.recv(65536)
                except OSError:
                    data = b''
                if not data:
                    running = False
                else:
                    os.write(fd, data)
                    if verbose: sys.stderr.write(f"[rx {len(data)}]\n"); sys.stderr.flush()
    except OSError as e:
        if verbose: sys.stderr.write(f"pump stopped: {e}\n")
    finally:
        try: termios.tcsetattr(fd, termios.TCSANOW, termios.tcgetattr(fd))
        except Exception: pass

errno_EAGAIN = 11  # EAGAIN/EWOULDBLOCK

def run_tcp(port, uart, baud, addr):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((addr, port)); srv.listen(1)
    sys.stderr.write(f"ad4-serial-bridge: TCP listen {addr}:{port} <-> {uart} @{baud}\n")
    while True:
        cli, peer = srv.accept()
        sys.stderr.write(f"client {peer} connected\n"); sys.stderr.flush()
        fd = setup_uart(uart, baud)
        try:
            pump(fd, cli)
        finally:
            os.close(fd); cli.close()
        sys.stderr.write("client disconnected\n")

def run_udp(port, uart, baud, addr):
    srv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    srv.bind((addr, port))
    sys.stderr.write(f"ad4-serial-bridge: UDP listen {addr}:{port} <-> {uart} @{baud}\n")
    peer = None
    fd = setup_uart(uart, baud)
    try:
        while True:
            r, _, _ = select.select([fd, srv], [], [], 1.0)
            if fd in r:
                try: data = os.read(fd, 65536)
                except OSError: continue
                if data and peer: srv.sendto(data, peer)
            if srv in r:
                data, peer = srv.recvfrom(65536)
                if data: os.write(fd, data)
    finally:
        os.close(fd)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tcp", action="store_true", default=True)
    ap.add_argument("--udp", action="store_true")
    ap.add_argument("--port", type=int, default=5000)
    ap.add_argument("--addr", default="0.0.0.0")
    ap.add_argument("--uart", default="/dev/ttyS1")
    ap.add_argument("--baud", type=int, default=250000)
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    if a.udp: run_udp(a.port, a.uart, a.baud, a.addr)
    else:     run_tcp(a.port, a.uart, a.baud, a.addr)

if __name__ == "__main__":
    main()
