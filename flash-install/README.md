# flash-install

One folder = one USB stick. Everything needed to take a stock Adventurer 4
to Klipper (and back) lives here; `flashdisk/` is the older scratch copy.

## Build the stick

```sh
./build-stick.sh /Volumes/MYSTICK            # stick must already be FAT32
```

Options (environment variables):

| var | effect |
| :-- | :-- |
| `AD4_ROOT_PASS='pw'` | writes `ad4-access.conf` with that root password (never stored in git) |
| `NO_FLASH=1` | do not touch the MCU — SoC access + bridge only |
| `NO_PERSISTENT=1` | one-shot stick, no eMMC changes |
| `RESTORE=1` | reflash `factory-HD.hex` and unwind the boot hook |

## At boot the stick does, in order

1. kills the vendor app so `/dev/ttyS1` is free,
2. flashes the MCU with `klipper.hex` through the vendor `IAPCommand`
   (refuses any image based at or below `0x00008000` — the HC32F460
   bootloader is flash-resident, that guard is what keeps the board alive),
3. sets `eth0` to `192.168.10.10/24`,
4. starts `ad4-bridge` (TCP `:5000` <-> `/dev/ttyS1` @115200),
5. installs `dropbear` + sets the root password,
6. with the `PERSISTENT` flag: copies the package to `/opt/ad4-klipper`,
   gates the vendor UI out of `/opt/auto_run.sh`, and reboots.

Everything is logged to `ad4-flash.log` **on the stick** — pull it and read
that file if the printer seems unresponsive.

## Factory apps

Neutralization is one `awk` wrap of the vendor launch line in
`/opt/auto_run.sh`:

```sh
if [ -f /opt/ad4-klipper/enable ]; then /opt/ad4-klipper/ad4-daemon.sh & \
else /opt/PROGRAM/ffstartup-arm -f /opt/PROGRAM/ffstartup.cfg & fi
```

So the toggle is the flag file, no re-patch needed:

```sh
rm    /opt/ad4-klipper/enable   # vendor UI back on next boot
touch /opt/ad4-klipper/enable   # Klipper daemon back
```

Full revert: build the stick with `RESTORE=1` (reflashes the stock MCU image
and runs `ad4-uninstall-persistent.sh --no-mcu` for you). The untouched
original is at `/opt/auto_run.sh.ad4-orig`.

## Root password

```sh
sh ad4-setrootpw.sh 'newpass'          # on the SoC
```

or over SSH from the Mac:

```sh
sshpass -p OLD ssh -o StrictHostKeyChecking=no root@192.168.10.10 \
    'cat > /tmp/ad4-setrootpw.sh' < ad4-setrootpw.sh
sshpass -p OLD ssh -o StrictHostKeyChecking=no root@192.168.10.10 \
    "sh /tmp/ad4-setrootpw.sh 'newpass'"
```

`flashforge_init.sh` re-applies `ROOT_PASS` on every stick boot, so a lost
password is one stick-boot away. Default is `flashforge`.

## Files

| file | what |
| :-- | :-- |
| `flashforge_init.sh` | the SoC-side script the vendor boot path runs from the stick |
| `build-stick.sh` | assembles the stick from this folder |
| `klipper.hex` | Klipper for HC32F460, positioned @ `0x0000C000`, PA9/PA10 @115200 |
| `factory-HD.hex` | stock HD app image (`Adventurer4-HD-IAP-2.3.0-20230216`), the restore image |
| `IAPCommand` | vendor ARM flasher; the only tool that speaks this bootloader |
| `ad4-bridge` / `.c` | static ARM (zig) TCP<->UART relay — the bridge |
| `dropbear` | dropbearmulti 2022.83 (dropbear/dropbearkey/ssh/scp) |
| `ad4-daemon.sh` | boot-time net + bridge, what the persistent hook runs |
| `ad4-install-persistent.sh` | installs the eMMC boot hook, then reboots |
| `ad4-uninstall-persistent.sh` | restores stock boot |
| `ad4-setrootpw.sh` | root password change + SSH persistence check |
| `ad4-revcheck.sh` | read-only NA-vs-HD MCU probe (`07`=HD, `06`=NA) |
| `ad4-net.conf` | `SOC_IP` / `SOC_NET` / `BRIDGE_PORT` |
| `screwflag` | empty; skips the vendor machine-type check |
| `ad4-serial-bridge.py` | python fallback (the SoC has no python3 — dead weight) |

## Rebuilding klipper.hex

```sh
cd ~/klipper-hc32 && make                    # board.config = hc32f460, PA9/PA10, 115200
python3 -I ~/adv4-klip/tools/make-positioned-ihex.py \
        out/klipper.bin 0xC000 ~/adv4-klip/flash-install/klipper.hex
```

Do **not** ship `out/klipper-ad4.hex` (what `make` leaves behind). It is `objcopy`
output, and for an image that straddles a 64K page `objcopy` switches to
type-02 *extended segment* records instead of type-04 — a loader that ignores
those writes the second page at `0x00000000`, straight over the flash-resident
bootloader. `make-positioned-ihex.py` emits one type-04 record per page, which
is the layout the factory image and `IAPCommand` are proven to consume.

Current `klipper.hex`: 28488 bytes @ `0x0000C000..0x00012F48`,
`sha256 00c3c429d1ff978b2e92bc59c586a88fb41cb4106cc969a9e0ac202d83c218eb`.

## After the stick

```sh
ssh root@192.168.10.10                      # password: whatever you set
sh /opt/ad4-klipper/ad4-revcheck.sh         # MCU identity (kills the bridge)
```

On the Mac the bridge is used directly: `[mcu] serial: socket://192.168.10.10:5000`
(see `../printer-hc32.cfg`).
