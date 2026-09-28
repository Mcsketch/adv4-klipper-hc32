# AD4 Klipper flash disk

Copy everything in this folder to the **root of a FAT32 USB stick**:

    flashforge_init.sh   (chmod +x before copying; FAT does not keep it)
    screwflag            (empty file)
    IAPCommand           (factory binary, from control-2.3.0/)
    klipper.hex          (from /Users/admin/klipper-hc32/out/)
    factory-HD.hex       (extracted from Adventurer4-HD-IAP-2.3.0-20230216.hex)

Restore mode: create an empty `RESTORE` file on the stick → the script
flashes `factory-HD.hex` instead of `klipper.hex`.

The SoC boot/update path executes `flashforge_init.sh` from removable media
(same mechanism/name as the factory updater, which also takes args
`Adventurer4 0012`). Exact trigger hook: verify once on hardware.

Result is logged to `/root/ad4-flash-status` on the SoC.
