# ALL-FROM-PI — SUPERSEDED

Historical design doc (early 2026): how to install remotely from a Raspberry
Pi when the stock AD4 has no SSH. **The actual unit turned out to already
offer SSH** (`ssh root@192.168.10.10`, password `flashforge`), and the Klipper
host is the **Mac**, not an RPi — so this whole path was never needed.

Current workflow: `FIRST-RUN-MANUAL.md` (ssh + IAPCommand + ad4-mac-start.sh)
and `PERSISTENT-INSTALL.md`. The root-password DES brute described below the
original text was abandoned as moot; the working credential is above.

Kept only for the firmware-archive analysis it cites (stock boot order,
telnetd/webUI/WiFi-AP findings), which still informs `adv4-soc-hack.md`.
