#!/usr/bin/env python3
# =============================================================================
# ad4-pi-telnet.py — run the WHOLE persistent Klipper install on the SoC from
# the RPi over TELNET (port 23), with NO USB stick and NO SSH.
#
# Why telnet: on a GENUINE (stock) AD4 there is no SSH (dropbear is a post-hack
# addition). The stock OS ships a telnetd (init.d entry "-x /usr/sbin/telnetd
# -- -F"). The login gate is the root password, which is a WEAK DES hash
# (H9.zxVgLFpRG2) — brute-forced offline. Once the password is known, telnet
# is the cleanest remote root shell on a stock unit.
#
# What it does, in ONE telnet session:
#   1. login root / <password>
#   2. mkdir /opt/ad4-klipper
#   3. stream the persistent package (base64) into /opt/ad4-klipper/pkg.b64
#      (base64 is pure ASCII -> telnet IAC-safe, no binary mangling)
#   4. base64 -d | tar xz  -> restore the real package files on the SoC
#   5. verify the transferred tgz by md5
#   6. run ad4-install-persistent.sh  (gates rcS, creates the enable flag)
#   7. flash Klipper to the MCU (IAPCommand, runs ON the SoC)
#   8. reboot (boot #2 is autonomous: vendor stack off, bridge up)
#
# Usage (on the RPi):
#   python3 ad4-pi-telnet.py --ip 192.168.10.10 --pass 'ROOT_PW'
#   python3 ad4-pi-telnet.py --ip 192.168.10.10 --pass 'ROOT_PW' --port 23
#   python3 ad4-pi-telnet.py --ip 192.168.10.10 --pass 'ROOT_PW' --dry-run
#
# --dry-run: build + stream the package but STOP before the installer/flash/
#   reboot (lets you verify the telnet channel + file transfer first).
#
# SAFETY: same reversible rcS gate as the stick path. If anything fails or the
#   enable flag is absent, the SoC boots exactly as stock. No eMMC is deleted.
# =============================================================================
import argparse, base64, hashlib, os, re, socket, sys, time, tarfile, io

IAC, WILL, WONT, DO, DONT, SB, SE = 255, 251, 252, 253, 254, 250, 240
EOT = 0x04
LF, CR = 0x0A, 0x0D

def strip_iac(data: bytes) -> bytes:
    """Remove IAC negotiation sequences; we answer every option with DONT/WONT."""
    out = bytearray(); i = 0; n = len(data)
    while i < n:
        if data[i] == IAC:
            if i + 2 < n:
                c = data[i+1]
                if c == IAC:
                    out.append(IAC)   # IAC IAC = one literal 0xFF data byte
                    i += 2; continue
                if c in (WILL, WONT, DO, DONT):
                    i += 3          # IAC + cmd + opt
                    continue
                elif c in (SB,):
                    # sub-negotiation: find terminating IAC SE
                    j = i + 2
                    while j < n - 1 and not (data[j] == IAC and data[j+1] == SE):
                        j += 1
                    i = j + 2
                    continue
                elif c == SE:
                    i += 2; continue
                i += 2; continue
            else:
                i += 1; continue
        out.append(data[i]); i += 1
    return bytes(out)

class Telnet:
    def __init__(self, host, port, timeout=15):
        self.s = socket.create_connection((host, port), timeout=timeout)
        self.s.settimeout(timeout)
        self.buf = b""
        # initial greeting: negotiate
        time.sleep(0.4); self._drain(1.0)

    def _raw_send(self, b): self.s.sendall(b)

    def _respond_iac(self, data):
        """Walk data; for each IAC WILL/DO reply WONT/DONT; WONT/DONT ignored."""
        i = 0; n = len(data)
        while i < n:
            if data[i] == IAC and i + 2 < n:
                c, opt = data[i+1], data[i+2]
                if c == WILL: self._raw_send(bytes([IAC, WONT, opt]))
                elif c == DO:   self._raw_send(bytes([IAC, DONT, opt]))
                i += 3
            else:
                i += 1

    def _drain(self, max_wait):
        end = time.time() + max_wait
        while time.time() < end:
            try:
                chunk = self.s.recv(4096)
            except socket.timeout:
                break
            if not chunk: break
            self._respond_iac(chunk)
            self.buf += chunk

    def read_until(self, patterns, timeout=20, partial=False):
        """Read until any pattern matches. Patterns are bytes (literal) or
        compiled regex. Returns (matched_index_or_None, accumulated_text_str)."""
        rx = []
        for p in patterns:
            if isinstance(p, re.Pattern):
                rx.append(p)
            else:
                rx.append(re.compile(p))
        end = time.time() + timeout
        while time.time() < end:
            blob = strip_iac(self.buf)
            for idx, r in enumerate(rx):
                if r.search(blob):
                    return (idx, blob.decode(errors="replace"))
            self.buf = b""
            try:
                chunk = self.s.recv(4096)
                if not chunk:
                    break
            except socket.timeout:
                continue
            self._respond_iac(chunk)
            self.buf += chunk
        blob = strip_iac(self.buf)
        for idx, r in enumerate(rx):
            if r.search(blob):
                return (idx, blob.decode(errors="replace"))
        return (None, blob.decode(errors="replace"))

    def sendline(self, s): self._raw_send(s.encode() + b"\r\n")

    def send_raw(self, b): self._raw_send(b)

    def run(self, cmd, until, timeout=30, settle=0.3):
        """Send cmd, wait until a pattern appears, return accumulated text."""
        self.sendline(cmd)
        time.sleep(settle)
        _, text = self.read_until(until, timeout=timeout)
        return text

    def close(self):
        try: self.s.close()
        except Exception: pass

def login(t: Telnet, user, password, timeout=25):
    # login prompt
    _, text = t.read_until([b"ogin", b"ogin:", b"login", b"user", b"user name", b"og in", b"ogin name"], timeout=timeout)
    time.sleep(0.3)
    t.sendline(user)
    # password prompt
    _, text = t.read_until([b"assword", b"assword:"], timeout=timeout)
    time.sleep(0.3)
    t.sendline(password)
    # shell prompt
    _, text = t.read_until([b"# ", b"$ ", b"root@", b"~ #", b"~\\$"], timeout=timeout)
    return text

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", required=True)
    ap.add_argument("--pass", dest="pw", required=True)
    ap.add_argument("--user", default="root")
    ap.add_argument("--port", type=int, default=23)
    ap.add_argument("--repo", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    ap.add_argument("--dry-run", action="store_true", help="transfer only; skip install/flash/reboot")
    ap.add_argument("--no-flash", action="store_true", help="install hook but skip MCU flash + reboot")
    a = ap.parse_args()

    repo = a.repo
    PKG = "/opt/ad4-klipper"
    tgz_path = "/tmp/ad4-pi-pkg.tgz"

    # ---- build the package locally (same file set as the SSH path) --------
    # ad4-net.conf is OPTIONAL: if absent we synthesize the standard 4-line
    # default (matching build-usb-stick.sh) so the telnet path never needs it
    # to pre-exist on disk.
    NETCONF_DEFAULT = (
        "# AD4 SoC network (static; the RPi host is 192.168.10.1)\n"
        "SOC_IP=192.168.10.10\nSOC_NET=24\nBRIDGE_PORT=5000\n"
    )
    members = [
        ("ad4-daemon.sh",            "flashdisk/ad4-daemon.sh",            True),
        ("ad4-install-persistent.sh","flashdisk/ad4-install-persistent.sh", True),
        ("ad4-uninstall-persistent.sh","flashdisk/ad4-uninstall-persistent.sh", True),
        ("ad4-serial-bridge.py",     "soc-hack/ad4-serial-bridge.py",       True),
        ("klipper.hex",              "stm32isp-ad4/klipper-ad4.hex",        True),
        ("IAPCommand",               "work/ctrl/IAPCommand",                True),
        ("ad4-net.conf",             "flashdisk/ad4-net.conf",              False),
    ]
    print("==> building package tgz")
    b64 = io.BytesIO()
    with tarfile.open(fileobj=b64, mode="w:gz") as tf:
        for arc, rel, required in members:
            full = os.path.join(repo, rel)
            if os.path.isfile(full):
                tf.add(full, arcname=arc)
            elif not required:
                import tarfile as _tf
                data = NETCONF_DEFAULT.encode()
                info = _tf.TarInfo(name=arc)
                info.size = len(data)
                info.mtime = 0
                tf.addfile(info, io.BytesIO(data))
                print(f"  (synthesized {arc} from defaults)")
            else:
                print(f"  MISSING {full} — cannot continue"); return 1
    b64.seek(0)
    pkg_bytes = b64.read()
    pkg_md5 = hashlib.md5(pkg_bytes).hexdigest()
    b64text = base64.b64encode(pkg_bytes).decode()
    print(f"  package: {len(pkg_bytes):,} bytes, md5={pkg_md5}, base64={len(b64text):,} chars")

    # ---- connect + login ---------------------------------------------------
    print(f"==> telnet {a.user}@{a.ip}:{a.port}")
    t = Telnet(a.ip, a.port)
    try:
        print("==> logging in")
        login(t, a.user, a.pw)
        t.run("echo AD4_TELNET_OK_$$", [b"AD4_TELNET_OK"], timeout=10)
        print("  root shell OK")

        # ---- transfer the package ------------------------------------------
        t.run(f"mkdir -p {PKG}", [b"# ", b"$ "], timeout=10)
        print(f"==> streaming package to {PKG}/pkg.tgz.b64")
        # enter cat, stream base64 (ASCII/IAC-safe), EOT
        t.sendline(f"cat > {PKG}/pkg.tgz.b64")
        time.sleep(0.4)
        # stream in chunks; drain echo between chunks
        chunk = 8000
        for i in range(0, len(b64text), chunk):
            t.send_raw(b64text[i:i+chunk].encode() + b"\r\n")
            time.sleep(0.05)
            t._drain(0.15)
        # EOF for cat
        t.send_raw(bytes([EOT]))
        time.sleep(0.5); t._drain(1.0)
        t.sendline("")
        t.run("echo B64DONE", [b"B64DONE"], timeout=15)
        # decode + verify md5 on the SoC
        t.run(f"base64 -d {PKG}/pkg.tgz.b64 > {PKG}/pkg.tgz", [b"# ", b"$ "], timeout=20)
        soc_md5 = t.run(f"md5sum {PKG}/pkg.tgz", [b"# ", b"$ "], timeout=15)
        if pkg_md5 not in soc_md5:
            print(f"  MD5 MISMATCH (expected {pkg_md5})\n  SoC said: {soc_md5.strip()}")
            print("  aborting — do NOT install a corrupt package."); return 2
        print(f"  md5 verified on SoC: {pkg_md5}")
        t.run(f"tar xz -C {PKG} -f {PKG}/pkg.tgz && rm -f {PKG}/pkg.tgz {PKG}/pkg.tgz.b64", [b"# ", b"$ "], timeout=25)
        t.run(f"chmod +x {PKG}/ad4-daemon.sh {PKG}/IAPCommand 2>/dev/null; ls -la {PKG}", [b"# ", b"$ "], timeout=15)

        if a.dry_run:
            print("\n== DRY-RUN: package transferred + verified. Stopping before install/flash. ==")
            print(f"   When ready, re-run without --dry-run to install + flash + reboot.")
            return 0

        # ---- install the persistent hook -----------------------------------
        print("==> installing persistent hook (gates rcS, enables)")
        t.run(f"sh {PKG}/ad4-install-persistent.sh", [b"# ", b"$ "], timeout=120)

        if a.no_flash:
            print("\n== --no-flash: hook installed, skipping MCU flash + reboot. ==")
            return 0

        # ---- flash Klipper to the MCU (runs ON the SoC) --------------------
        print("==> flashing Klipper to the MCU (on the SoC)")
        t.run(f"cd {PKG} && pkill -f ffstartup-arm 2>/dev/null; pkill -f adventurer4-arm 2>/dev/null; sleep 2; ./IAPCommand klipper.hex klipper.hex; echo flash-rc=$?", [b"flash-rc="], timeout=120)

        # ---- reboot ---------------------------------------------------------
        print("==> rebooting the SoC (boot #2 is autonomous)")
        t.sendline("sync; sleep 2; (reboot 2>/dev/null || reboot -f 2>/dev/null || /sbin/reboot 2>/dev/null) &")
        time.sleep(2)
        print("\nDONE. When the SoC is back (~30-60s):")
        print(f"  ping {a.ip}")
        print(f"  nc -zv {a.ip} 5000     # bridge up")
        print("  ./soc-hack/ad4-rpi-setup.sh   # RPi side (once), then Fluidd")
        return 0
    except Exception as e:
        print(f"\nERROR: {type(e).__name__}: {e}")
        return 3
    finally:
        t.close()

if __name__ == "__main__":
    sys.exit(main())
