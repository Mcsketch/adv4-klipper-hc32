# Q5: Bed levelling

## What the stock firmware did

The AD4 has no levelling sensor. Its "Nine Point Auto Calibration" is a paper
test at 9 points, and the screen app applies the result to every job.

Evidence, from the stock screen app (`adventurer4-arm`, not stripped) and the
MCU firmware:

- `AutoCalibration::on_pointN_offset_clicked` sends `G1 Z10 F300`, moves to
  the point (`G1 X-110 Y95` and so on, 3 x 3 at X -110 / 0 / 100,
  Y 95 / 0 / -95), then `G1 Z` to the point's saved height.
- `on_Zm_pressed` / `on_Zp_pressed` jog Z in 0.05 or 0.5 mm steps
  (`G1 Z… F300`); `getZaxis` reads the result back with `M114`.
- `checkMove` polls `M119`. The MCU's only endstop report is
  `Endstop: X-max Y-max Z-max`: there is no probe input.
- The 9 heights go to `autoCalibration.point1…9` in
  `/opt/finder_rush/config/config-AD4.json` on the SoC.
  `AutoCalibrationAlgorithm::AnalysisGcodeOffset` rewrites each job's Z with
  them while `status` is true.

Some shop pages describe an inductive sensor on the extruder. This unit's
firmware has no code for one.

## What that means for Klipper

Nothing to buy. Klipper does the same paper test:
`BED_MESH_CALIBRATE METHOD=manual` (any `BED_MESH_CALIBRATE` falls back to it
when there is no `[probe]`). It visits the 3 x 3 points of `[bed_mesh]`. At
each one, slide paper under the nozzle and step Z with `TESTZ` until it just
drags, then `ACCEPT`. Mainsail shows a dialog for this. `[bed_mesh]` now
covers X 10–210, Y 10–190 at 60 mm/s.

## The stock values, as a Klipper mesh

Caleb's unit still has its stock values. Coordinates: X shares the stock
X-max switch, so Klipper X = stock X + 110. The Y switch is stock Y-max and
Klipper's Y 4, so Klipper Y = 100 − stock Y. Z: both raise the nozzle with
+Z, and Klipper's Z0 was set by a paper test at the centre. Each value is
the stock height minus the centre point's (−3.54).

| Klipper Y \ X | 0 | 110 | 210 |
|---|---|---|---|
| 5 | −0.21 (p1) | −0.01 (p2) | +0.04 (p3) |
| 100 | −0.26 (p6) | 0.00 (p5) | +0.04 (p4) |
| 195 | −0.21 (p7) | −0.01 (p8) | +0.09 (p9) |

That is a bed about 0.25 mm low along the left edge (X 0) and flat elsewhere
within 0.1 mm. The live printer.cfg has it as `[bed_mesh stock]` (not in the
repo copy; it belongs to this unit). Nothing loads it. The values date from
the stock firmware and may not match the plate now.

On another unit: read the 9 points from the same file and build the same
table.

## Test (with Caleb at the printer)

1. Heat the bed to 60, `G28`.
2. `BED_MESH_CALIBRATE METHOD=manual`, paper test at the 9 points.
3. Compare with the table above. `BED_MESH_PROFILE SAVE=default`, then
   `SAVE_CONFIG` (restarts Klipper).
4. Print a single-layer plate (9 squares at the mesh points) with
   `BED_MESH_PROFILE LOAD=default` before it. If the left squares look
   better, add the load to `PRINT_START` after homing.

A real probe can come later, if a manual mesh drifts too often. It would
need a free MCU input and a mount.

## Result, 2026-11-14

Manual mesh with Caleb (cold nozzle, bed 55), normalised to 0 at X110 Y100,
saved as `[bed_mesh default]` in the live config. `PRINT_START` loads it after
`G28` when a `default` profile exists; `zero_reference_position: 110, 100`
normalises future runs.

| Y \\ X | 10 | 110 | 210 |
|---|---|---|---|
| 190 | -0.153 | +0.101 | +0.199 |
| 100 | -0.203 | 0.000 | +0.098 |
| 10 | -0.156 | +0.051 | +0.152 |

Left edge low as in the stock table (-0.21 to -0.26), right side 0.1-0.2
higher than stock: the bed tilts about 0.35 mm across X. Left: the 9-square
first-layer plate, once the new nozzle (Q9) extrudes cleanly.
