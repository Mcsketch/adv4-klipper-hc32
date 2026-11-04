# Q1: Pressure advance

Klipper's tuning tower method (`docs/Pressure_Advance.md` in Klipper): print a
hollow square tower while Klipper raises pressure advance with height, then
read the height where the corners look best.

## The job

`ad4_cal_pa_tower.gcode` on Moonraker, built by
[`calibration/pa_tower_slice.py`](calibration/pa_tower_slice.py) from
Klipper's `docs/prints/square_tower.stl` (66 x 66 x 50 mm) with the
AD4 PLA Standard / Rapid PLA+ presets and these changes: 1 wall, 2 bottom
layers, no infill, no top, walls asked at 60 mm/s (the 3 mm³/s flow cap holds
them near 36), no slicer acceleration commands, no layer-time slowdown. After
`PRINT_START` it adds:

```
SET_VELOCITY_LIMIT SQUARE_CORNER_VELOCITY=1 ACCEL=500
TUNING_TOWER COMMAND=SET_PRESSURE_ADVANCE PARAMETER=ADVANCE START=0 FACTOR=.005
```

So PA is 0 at the bed and 0.25 at the top (0.005 per mm). About 1 h 12 min,
3.7 m of filament.

`PRINT_START` puts speed, flow, acceleration, corner velocity and PA back to
printer.cfg before every job, so nothing from the tower leaks into the next
print, even after a cancel. The tower test itself ends at the next job's
purge line.

## Reading it

1. Look at the four corners with a light from the side. Low on the tower the
   corners bulge (too little PA); high up they round off or show gaps right
   after the corner (too much).
2. Find the height where the corners are sharpest. Measure it from the bed
   side with calipers.
3. PA = height x 0.005. Example: best at 12 mm -> 0.06.
4. Set it as `pressure_advance:` under `[extruder]` in printer.cfg and
   `RESTART`. For a filament that needs its own value, put
   `SET_PRESSURE_ADVANCE ADVANCE=<value>` in that filament preset's start
   G-code: Orca runs it after the machine start G-code, so after the reset.

The 2026-11-03 ladder kept 0.05 and ruled out 0.5 (thin walls, confounded by
heat creep, see RUNBOOK section 7). A value outside 0.02-0.15 on this tower
means re-check the reading before using it.
