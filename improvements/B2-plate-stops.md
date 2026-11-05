# B2: Plate stops

One plate, several parts printed one after another, and the printer stops
between them. Nobody clears the plate: set up the next part (swap the
filament, put in magnets, check the last part), tap READY, and it carries on.
The same stop works mid-part at a chosen layer.

## How it works

The slicer does the layout. Orca's plate setting **print sequence: by object**
prints each object to its full height before the next. Its extruder
clearance (printer settings) draws the area around each object that the head
needs, so parts placed outside those areas cannot be hit later. It also
refuses a plate where a part other than the last is taller than the gantry
clearance.

What Orca 2.4.2 writes between two objects (checked by slicing three cubes
8, 20 and 5 mm tall):

1. It prints them shortest first (5, 8, 20).
2. After an object: retract 5.5 mm, lift, `EXCLUDE_OBJECT_END`, travel to
   the next object's centre at the last height + 0.4 mm.
3. `M104` at the job's temperature, then **Between objects G-code**, then the
   first layer of the next object (`G1 X Y Z0.3`, `G1 E5.5`).

`SET_PRINT_STATS_INFO CURRENT_LAYER` keeps counting across objects, so the
panel's layer N / M is the whole plate.

The printer side is in `host/printer.cfg`:

| Macro | |
|---|---|
| `PLATE_NEXT_OBJECT` | Orca's between-objects hook. Sets the panel message "Next: object N of M", then `PAUSE`. |
| `PLATE_PAUSE_LAYER` | Orca's pause G-code. `MSG=` sets the message; without it "Layer pause". |
| `PAUSE` | Replaces mainsail.cfg's. Remembers the tallest Z this job has reached, retracts 1 mm, lifts to 10 mm above that (capped 2 mm below Z max) and brings the bed forward. Raises the idle timeout to 1 h. |
| `RESUME` | Goes back over the part at that safe height, primes 1 mm, puts the idle timeout back, clears the message, then `RESUME_BASE` drops straight down to where it stopped. |

`PRINT_START` resets the tallest Z, the object counter and the message.

Why the idle timeout: Klipper's (10 min) turns heaters and motors off even
while paused. With the motors off the head's position is lost and the job
cannot resume. During a stop it is 1 h; after that the job is dead, as
before. The nozzle stays hot during a stop; the filament sits 6.5 mm back
(Orca's 5.5 mm retract + PAUSE's 1), which keeps the ooze down.

A filament swap during a stop: `LOAD_FILAMENT` (and the panel's purge) end
with the same 6.5 mm pulled back while a job is paused (`swap_retract` in
`PAUSE`), so RESUME's 1 mm and Orca's 5.5 mm unretract put the new filament
at the tip instead of pushing a blob. If the slicer's retraction length
changes, change `swap_retract` with it.

## The panel

While paused the home screen shows the message (amber), the thumbnail, and
"Set up, then tap READY." The buttons are READY (RESUME on a plain pause),
TUNE, CANCEL and FILAMENT. Filament during a job is load, unload and purge at
the job's own temperature; the temperature and cooldown buttons are hidden,
so the job resumes at the slicer's temperature. The phone notification for
a pause carries the same message.

A failed part: tap REDO on the stop card (twice). `PLATE_REDO` purges 20 mm,
rewinds the job to the start of the current part (at a between-objects stop,
the part just finished) and shows "Redo object N". Take the failed part off
(on the first part also the skirt), tap READY, and the part prints again from
its first layer. Roadmap B4.

## Using it

Plate of parts:

1. Plate settings (the plate's gear icon) > print sequence: **By object**.
2. Lay the parts out; keep each outside the others' clearance outline.
3. Slice, print. Between parts the printer stops; set up, tap READY.

Pause at a layer (magnets, nuts, a colour change):

1. In the preview, drag the layer slider to the layer **above** the pocket's
   last open layer, right-click the + > **Add pause**. It runs
   `PLATE_PAUSE_LAYER` before that layer.
2. For a message on the panel use **Add custom G-code** instead:
   `PLATE_PAUSE_LAYER MSG="Insert 6x3 magnets"`.

## Before the first real plate

- [x] **Head clearance.** Measured on Caleb's unit: the head is 50 mm wide
      and 80 mm deep, the nozzle 30 mm from its left side and 50 mm from its
      back, so the farthest corner is √(30² + 50²) = 58.3 mm away. The
      preset's radius is 60 (Orca's AD4 profile has 42.3, which lets the
      back of the head reach a part). Nozzle tip to the bottom of the X rods
      is about 30 mm; the preset keeps Orca's 24.93 for height to rod, the
      lower of the two. Height to lid stays 250 (the build height).
- [ ] Test with Caleb at the printer: two or three 10 mm cubes by object,
      one stop with a filament swap, one `PLATE_PAUSE_LAYER` with a message.
      Watch the lift and the return over the part.

## Later

- New jobs on a plate that already has parts on it: the printer would need
  to know what is still standing (from the last job's `exclude_object`
  outlines and heights) and refuse a job that overlaps them.
