# OrcaSlicer profiles for the AD4 under Klipper (S2)

Tested with OrcaSlicer 2.4.2. Each preset inherits Orca's own Flashforge
Adventurer 4 profile and changes only what Klipper and this printer need.

| File | |
|---|---|
| `orca/machine/AD4 0.4 Nozzle (Caleb).json` | Klipper flavour, `SET_PRINT_STATS_INFO` total and current layer (the panel's layer N / M), 220 x 200 bed with origin at 0,0, 60 mm/s and 2000 mm/s² limits, retraction 5.5 mm at 25 mm/s, `PRINT_START BED= EXTRUDER=` / `PRINT_END`, between-objects `AD4_NEXT_OBJECT` and pause `AD4_PAUSE_LAYER` (plate stops, B2), extruder clearance radius 60 mm (measured; height to rod stays Orca's 24.93), upload to Moonraker (Orca's OctoPrint host type), 32x32 and 160x160 PNG thumbnails |
| `orca/process/AD4 PLA *.json` | Standard, Cosmetic, Fast Draft, Structural. All have object labels and `EXCLUDE_OBJECT` on, all speeds at or under 60 mm/s, and klipper_estimator as the post-processing step |
| `orca/filament/AD4 *.json` | Rapid PLA+ (225 °C, fan 40 %) and SUNLU PLA (flow 0.96). Max volumetric speed 3 mm³/s, because the hot end slips near 4.8 |
| `orca/klipper_estimator_ad4.json` | This printer's limits for klipper_estimator, from `dump-config` |

## Install (Windows)

1. Copy the three folders into `%APPDATA%\OrcaSlicer\user\default\`. Caleb's
   ASRock does this with junctions to `C:\Users\caleb\FreeCAD\slicer\OrcaSlicer`
   instead.
2. In Orca's setup wizard, tick Flashforge > Adventurer 4 Series 0.4 nozzle.
   The user presets only show up once their parent printer is enabled.
3. Install [klipper_estimator](https://github.com/Annex-Engineering/klipper_estimator/releases)
   (v3.7.3) to `%LOCALAPPDATA%\klipper_estimator\klipper_estimator.exe` and put
   `klipper_estimator_ad4.json` where the process presets' `post_process` line
   points. If you keep them somewhere else, edit that line.
4. In the printer preset, set the host to your Mainsail address.

## Accurate time (J2)

klipper_estimator replays the G-code through Klipper's motion planner using
this printer's limits, then rewrites the slicer's time estimate and every
`M73`. On the two-object test plate, Orca estimated 22m 24s and
klipper_estimator 23m 33s. Neither number includes `PRINT_START`'s heating
and homing.

Re-run `dump-config` after changing any limit in printer.cfg:

```
klipper_estimator.exe --config_moonraker_url http://192.168.1.10:8095 dump-config > klipper_estimator_ad4.json
```

Orca's command line does not run `post_process` and does not render
thumbnails. Only a job sliced and exported from the GUI has both.
