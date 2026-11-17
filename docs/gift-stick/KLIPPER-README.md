# Klipper for the Adventurer 4 (optional, advanced)

Klipper is replacement firmware that Caleb built and ran on this exact
printer. It gives finer control (pressure advance, bed mesh, web interface),
but it **needs a second computer** running Klipper the whole time you print:
a Raspberry Pi, or a Linux laptop, wired to the printer by Ethernet.

**If you're happy with the stock printer, ignore this folder.**

## What's here

| File | What it's for |
|---|---|
| `ad4-klipper-install-*.zip` | Install stick: puts Klipper on the printer. |
| `ad4-stock-rollback-*.zip` | Rollback stick: puts FlashForge's stock firmware back. This printer was restored to stock with this exact stick. |
| `README.html` / `.md` | Overview of the project. |
| `INSTALL.md` | Full install steps: host, stick, first print. |
| `ROLLBACK.md` | Going back to stock. |
| `TROUBLESHOOTING.md` | Panel messages and Klipper errors. |
| `HOW-IT-WORKS.md` | Why it can't brick the printer. |
| `printer.cfg` | The Klipper config for this printer. |

## Using a zip

1. Get an **empty** FAT32 USB stick (not this one).
2. Unzip the zip onto it so that `flashforge_init.sh` sits at the **top
   level** of the stick, not inside a folder.
3. Printer off, stick in, power on, and follow the touchscreen.
4. When it's done, take the stick out. If a stick with `flashforge_init.sh`
   on it stays in the printer, it runs again at every boot.

Project source: <https://github.com/Mcsketch/adv4-klipper-hc32>
