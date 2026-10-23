# AD4 — First-Run Manual

How to go from a powered-off printer to a live Klipper stack. Everything here
is **proven on this unit** (2026-10-22): heaters, sensors and the serial chain
all verified working. The Klipper host is the **Mac** — no RPi needed.

## Topology

```
[HC32F460 MCU] --USART2 PA9/PA10 @115200--> SoC /dev/ttyS1
[SoC 192.168.10.10]  ad4-bridge :5000  <--TCP--  klippy on the Mac
```

The SoC does the MCU flashing (it owns NRST/BOOT0). The Mac runs
klippy + moonraker + Fluidd.

## 1. Reach the SoC

```sh
sshpass -p flashforge ssh -o StrictHostKeyChecking=no \
    -o PubkeyAuthentication=no root@192.168.10.10
```

BusyBox (no pkill/pgrep/python3). `/opt/ad4-klipper/` holds `IAPCommand`,
`ad4-bridge`, `ad4_identify`.

## 2. Make sure the bridge is up

```sh
cd /opt/ad4-klipper && ./ad4-bridge 5000 /dev/ttyS1 115200 &
```

Single-client: it frees the UART when the client (klippy) disconnects, so
flashing and running never fight over ttyS1. Note: the bridge does **not**
auto-start after a power-cycle yet — see PERSISTENT-INSTALL.md.

## 3. Flash the MCU (vendor IAPCommand — the proven path)

From the Mac, with klippy stopped (it must release the bridge):

```sh
/Users/admin/adv4-klip/bridge/ad4-mac-stop.sh
scp -O /Users/admin/klipper-hc32/out/klipper-ad4.hex root@192.168.10.10:/opt/ad4-klipper/
ssh ... 'cd /opt/ad4-klipper && ./IAPCommand klipper-ad4.hex klipper-ad4.hex /dev/ttyS1 115200'
```

Two hex args (NA + HD image) — that's the vendor tool's calling convention.
It never touches the bootloader region. Verify the sha256 on both sides.

## 4. If the MCU is unresponsive: NRST pulse

The SoC can hard-reset the MCU over GPIO:

```sh
echo 0 > /sys/class/gpio/gpio34/value; sleep 0.3; echo 1 > /sys/class/gpio/gpio34/value
```

(gpio34 = NRST, gpio35 = BOOT0 — keep BOOT0 at 0 or you boot ROM-ISP.)
Liveness check: `cd /opt/ad4-klipper && ./ad4_identify 115200 /dev/ttyS1`
— a healthy MCU answers with an identify blob.

## 5. Start the stack on the Mac

```sh
/Users/admin/adv4-klip/bridge/ad4-mac-start.sh
```

Starts klippy (`serial: socket://192.168.10.10:5000`), moonraker (:7125) and
Fluidd at **http://127.0.0.1:8081/** (strata keeps :8080). Stop with
`ad4-mac-stop.sh`. Send gcode with:

```sh
printf 'M105\n' > /Users/admin/adv4/links/gcode
```

## 6. First tests (all passed 2026-10-22)

- `M105` / Fluidd temps: hotend (PA0) and bed (PA5) read room temp.
- `M140 S60`: bed (PA7 heater) climbs ~22°C/min. `M140 S0` after.
- `M104 S60`: hotend (PA1 heater) reaches 60°C, PC2 fan auto-starts >50°C.
- **Stop before motion tests**: stepper/endstop pins are still unverified
  guesses — see STATUS.md.

## Recovery

- Bad flash: re-run step 3 (bootloader is never erased).
- MCU stuck: NRST pulse (step 4), then re-flash.
- Full factory restore: factory HD firmware is archived in
  `FlashForge-Hacks-main/Firmware/`; the USB-stick `flashforge_init.sh` path
  can re-flash it (see PERSISTENT-INSTALL.md).
