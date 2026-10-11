# AD4 Klipper flash disk (unattended USB)

The "plug in a USB stick and the printer does everything" path. One script on
the stick runs from the SoC's stock boot/update path and, in one go:

1. **Flashes the MCU** (Klipper, or factory if a `RESTORE` flag file is
   present) — with a base-address guard so a mis-positioned image can never
   clobber the ROM-ISP/reset region (never brickable).
2. **Hacks the SoC network** — static IP on `eth0` (default `192.168.10.10`).
3. **Starts the MCU bridge** — `/dev/ttyS1` ↔ TCP `:5000`, so the RPi host
   reaches the MCU over Ethernet with no soldering.

## Build the stick (on the Mac)

```sh
./build-usb-stick.sh /Volumes/MYSTICK
```

Format the stick FAT32 and mount it first. The builder verifies the flasher's
md5 and the Klipper hex's base address before it copies anything.

## What lands on the stick (disk root)

| File | Role |
|---|---|
| `flashforge_init.sh` | the SoC-side flash+net+bridge script (run at boot) |
| `screwflag` | empty file — skips the machine-type check |
| `IAPCommand` | factory ARM flasher, md5 `919864f7…`; it does the GPIO strapping (NRST=34, BOOT0=35, `/dev/ttyS1`) |
| `klipper.hex` | Klipper, positioned at `0x0000C000` (type-04 iHEX, byte-identical to `klipper.bin`) |
| `factory-HD.hex` | known-good full restore image (base `0x0000`, full chip) |
| `ad4-serial-bridge.py` | MCU `/dev/ttyS1` ↔ TCP `:5000` bridge (byte-transparent) |
| `ad4-net.conf` | `SOC_IP=192.168.10.10  SOC_NET=24  BRIDGE_PORT=5000` (editable) |

## Image select (by flag file, not filename)

- no `RESTORE` file → flash `klipper.hex`
- `RESTORE` present → flash `factory-HD.hex` (revert to stock)

The flasher takes a two-hex argv and auto-selects HD/NA from the sync reply
(`6` = HD, `7` = NA). We pass the same image to both slots, which is
rev-agnostic for a single known revision.

## Anti-brick guarantee (and the bug it fixed)

The MCU's ROM ISP lives at `0x00000000` and is **never** written, so it can
always re-enter ISP by repeating the GPIO strapping. On top of that,
`flashforge_init.sh` runs a base-address guard on the image before flashing:

- **Klipper image (strict):** must have lowest programmed address `> 0x00008000`.
- **Factory image (permissive):** a full-chip dump at base `0x0000` by design;
  accepted, sanity-checked for size.

**The bug this guard caught:** the stock `objcopy -O ihex --change-address`
does **not** emit extended-address records for an image that straddles a 64K
page, so the second page wrapped to 16-bit offsets and the flasher's loader
resolved the whole image to base `0x00000000` — which would have clobbered the
ROM-ISP region and **bricked** the printer. The fix is
`tools/make-positioned-ihex.py`, which emits proper type-04 records; the
canonical `stm32isp-ad4/klipper-ad4.hex` is produced that way and verified to
resolve to `0x0000C000`.

## After the stick runs

The SoC is now at `192.168.10.10` with the MCU bridged on `:5000`. On the RPi3
(set to `192.168.10.1`):

```sh
./ad4-rpi-setup.sh            # or with --moonraker for the web UI
```

Result is logged to `/root/ad4-flash-status` on the SoC.

## Revert to stock any time

`touch /path/to/stick/RESTORE`, re-plug the stick, power the printer. Or from
the SoC: `<repo>/stm32isp-ad4/flash-mcu.sh factory`.
