# Flashforge Adventurer 4: start here

This printer runs **FlashForge's stock firmware**, exactly as it came from
the factory. You use it with FlashForge's own slicer, FlashPrint, which is
on this stick.

## What's on this stick

| Folder | What it is |
|---|---|
| `1 Slicer - FlashPrint` | FlashPrint 5.8.7 for Windows. It turns 3D models into print files. |
| `2 Test prints` | Calibration models (STL). Slice them in FlashPrint first. |
| `3 Klipper (optional, advanced)` | Optional replacement firmware, plus the way back to stock. You don't need it. |

## 1. Set up the printer

1. Put it on a stable table and plug it in. Leave about 10 cm around it so
   the fan can breathe.
2. Power on. The touchscreen starts the FlashForge UI.
3. **Load filament:** on the touchscreen, open the filament menu and choose
   Load. The nozzle heats up. Push PLA into the hole on top of the print
   head until plastic comes out of the nozzle.
4. **Level the bed** with the touchscreen's levelling assistant. It moves
   the nozzle to each point and you slide a sheet of printer paper under it.
   The nozzle is at the right height when the paper drags a little: not
   loose, and not stuck. Adjust until every point feels the same.

## 2. Install the slicer

1. Copy `1 Slicer - FlashPrint\FlashPrint 5_5.8.7_x64.exe` to the laptop and
   run it.
2. On first start, choose the printer **Flashforge Adventurer 4**.
3. A newer version may exist:
   <https://www.flashforge.com/download-center/63>

## 3. First print

1. In FlashPrint, **Load** `2 Test prints\CalibrationCube.stl`.
2. Click **Print** (the slice button). Use material **PLA**, resolution
   **Standard**, and turn on **Raft** or **Brim** for the first few prints.
3. **Export** the result (a `.gx` file) to the **top level** of a USB stick.
   You can use this stick.
4. Plug the stick into the printer. On the touchscreen, open Print and
   choose the USB stick, then the file.
5. Watch the first layer. It should be flat and squished onto the plate. If
   it is round or doesn't stick, re-level the bed (step 1.4).

The printer can also receive files over WiFi or Ethernet from FlashPrint.
Connect it to the network from the touchscreen's settings first.

## Test prints, in order

| File | What it checks |
|---|---|
| `ParametricBedLevel.stl` | Is the bed level everywhere? Every square should look the same. |
| `CalibrationCube.stl` | Size: X, Y and Z should each measure 20 mm. |
| `TempTowerPLA.stl` | Best temperature for your PLA. Pick the cleanest level. |
| `Overhang.stl`, `BridgeTest.stl` | How steep an overhang and how long a bridge print without support. |
| `RetractTest.stl` | Stringing between towers. |
| `Tolerance.stl` | How much gap moving parts need. |
| `3DBenchy.stl` | The classic all-round test boat. |

## Tips that save prints

- **Clean the plate** with isopropyl alcohol or dish soap and water before
  printing. Finger grease is the number one reason prints come loose.
- If tall prints pop off partway through, add a **brim**, raise the bed
  temperature to 60 °C, or use a thin layer of glue stick.
- PLA at 200–220 °C works on this printer. **Keep speeds moderate**: the
  stock hot end can't melt plastic fast enough at high speed, and the
  extruder starts to click.
- Clicking from the extruder or thin, gappy lines means a clog or speed
  that's too high. Slow down first. If that doesn't help, use the
  touchscreen's filament unload/load to clear it.

## Help

- FlashForge support and manuals: <https://www.flashforge.com>
- Caleb set this up. Ask him.
