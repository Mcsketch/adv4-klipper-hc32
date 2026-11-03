# Rollback to stock

Puts FlashForge's firmware back: the stock MCU firmware, the stock boot
script, the stock touchscreen UI. Use it to go back for good, or to recover
from an install that failed halfway.

## Steps

1. Unzip `ad4-stock-rollback-VERSION.zip` onto an empty FAT32 stick
   (`flashforge_init.sh` and `ad4-klipper/` at the top level).
2. Printer off, stick in, power on.
3. The panel asks **Restore stock firmware?** Tap **RESTORE**.
4. A progress bar counts the 44 stock firmware frames. Do not power off.
5. **Stock firmware restored**, and the printer reboots into the FlashForge
   UI. Take the stick out.

## What it does

1. Checks FlashForge's MCU kit in `/opt/PROGRAM/control/` against its own
   `md5sum.list`. The stock firmware comes from there; nothing from
   FlashForge is on the stick.
2. Removes the Klipper boot flag first, so even if the next steps fail the
   printer does not boot into Klipper mode again.
3. Flashes the stock firmware with FlashForge's IAPCommand. IAPCommand
   picks the NA or HD image by asking the chip, so this works on both
   boards.
4. Restores the stock launch line in `/opt/auto_run.sh` (the original from
   before the install is also kept as `/opt/auto_run.sh.pre-ad4-klipper`).
5. Deletes `/opt/ad4-klipper/` and the SSH server the install added.

It works on a printer running Klipper, on one where the install stopped
halfway, and on a stock printer (where it just reflashes stock).

## Afterwards

- The root password set by the install stays; nothing else from the
  install is left. SSH is gone with the SSH server.
- On the Klipper host, nothing changes. Remove the printer from it, or keep
  it for the next install.

## If it fails

**Flash failed**: the MCU has no working firmware. Power the printer off and
on with the stick still in: the stick runs again and asks again. The
bootloader is never touched, so retrying is always possible. Keep
`ad4-klipper.log` and `iap.log` from the stick.

**Stock kit missing**: the printer's copy of the stock firmware is damaged.
Nothing was changed. Install FlashForge's 2.3 firmware update package from
their website through the stock UI first, or ask in the issues.
