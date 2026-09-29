# AD4 Klipper flash disk (unattended)

Copy everything in this folder to the **root of a FAT32 USB stick**:

    flashforge_init.sh   (chmod +x before copying; FAT does not keep the bit)
    screwflag            (empty file — skips the machine-type check)
    IAPCommand           MCU flasher. Either:
                            • the factory binary (from control-2.3.0/), or
                            • the built stm32isp-ad4 fork — compile it, then
                              `cp stm32isp-ad4/stm32isp-ad4  IAPCommand`
                          (the fork takes the same two-hex argv as the factory one)
    klipper.hex          Klipper, built for the HD (H32) app window (from /Users/admin/klipper-hc32/out/)
    factory-HD.hex       known-good restore image = Adventurer4-HD-IAP-2.3.0-20230216.hex

Image select is by **flag file**, not filename:

- no `RESTORE` file  → flash `klipper.hex`
- `RESTORE` present  → flash `factory-HD.hex` (revert to stock)

Why two hexes as IAPCommand args: the factory `IAPCommand` is built for two
positioned images (NA + HD) and picks the right one from the sync reply
(6 = HD, 7 = NA); the `stm32isp-ad4` fork keeps the same argv for drop-in
compatibility. See `adv4-exploration.md` §5–7.

The SoC boot/update path executes `flashforge_init.sh` from removable media
(same mechanism/name as the factory updater, which also takes args
`Adventurer4 0012`). Exact trigger hook: verify once on hardware.

Result is logged to `/root/ad4-flash-status` on the SoC.
