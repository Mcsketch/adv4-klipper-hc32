"""Slice the Q3/Q4 extrusion speed tower (OrcaSlicer CLI, run from WSL).
A 50 x 50 mm single-wall tube, walls at 30 mm/s; the job then raises M220 by
a third every 5 mm, so bands run 30, 40, 50 ... mm/s up to TOP (argv[1],
default 80). Paths are Caleb's ASRock, as in pa_tower_slice.py.
Output: S/cal/ad4_cal_flow_tower.gcode."""
import glob, json, os, struct, subprocess, sys
TOP = int(sys.argv[1]) if len(sys.argv) > 1 else 80
BASE, BAND = 30, 5
H = BAND * ((TOP - BASE) // 10 + 1)
P = "/mnt/c/Program Files/OrcaSlicer/resources/profiles"
U = "/mnt/c/Users/caleb/FreeCAD/slicer/OrcaSlicer"
S = "/mnt/c/Users/caleb/AppData/Local/Temp/ad4cube/s1"

def box(path, x, y, z):
    v = [(a, b, c) for a in (0, x) for b in (0, y) for c in (0, z)]
    q = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
    tris = [t for a, b, c, d in q for t in ((a, b, c), (a, c, d))]
    with open(path, "wb") as f:
        f.write(b"\0" * 80 + struct.pack("<I", len(tris)))
        for t in tris:
            f.write(struct.pack("<12fH", 0, 0, 0, *[c for i in t for c in v[i]], 0))
box(f"{S}/flow_tower.stl", 50, 50, H)

idx = {}
for p in glob.glob(P + "/Flashforge/**/*.json", recursive=True) + glob.glob(U + "/*/*.json"):
    j = json.load(open(p, encoding="utf-8"))
    if isinstance(j, dict) and "name" in j:
        idx[j["name"]] = j
def resolve(name):
    j = idx[name]
    base = resolve(j["inherits"]) if j.get("inherits") else {}
    base.update(j)
    return base
over = {"process": {"top_shell_layers": "0", "sparse_infill_density": "0%", "wall_loops": "1", "bottom_shell_layers": "2",
                    "outer_wall_speed": str(BASE), "inner_wall_speed": str(BASE), "internal_solid_infill_speed": str(BASE),
                    "post_process": [],
                    **{k: "0" for k in ("default_acceleration", "outer_wall_acceleration", "inner_wall_acceleration",
                        "internal_solid_infill_acceleration", "sparse_infill_acceleration", "travel_acceleration",
                        "top_surface_acceleration", "initial_layer_acceleration", "bridge_acceleration")}},
        "filament": {"slow_down_for_layer_cooling": ["0"], "filament_max_volumetric_speed": ["20"]}}
for kind, name in [("machine", "AD4 0.4 Nozzle (Caleb)"), ("process", "AD4 PLA Standard"), ("filament", "AD4 Rapid PLA+")]:
    j = resolve(name)
    j.update(over.get(kind, {}))
    j["inherits"] = idx[name]["inherits"]
    j["from"] = "User"; j["type"] = kind
    json.dump(j, open(f"{S}/f_{kind}.json", "w"), indent=1)
os.makedirs(S + "/cal", exist_ok=True)
r = subprocess.run(["/mnt/c/Program Files/OrcaSlicer/orca-slicer.exe", "--slice", "0", "--arrange", "1",
    "--load-settings", "f_machine.json;f_process.json", "--load-filaments", "f_filament.json",
    "--outputdir", "cal", "flow_tower.stl"], cwd=S, capture_output=True, text=True)
if r.returncode:
    sys.exit((r.stdout + r.stderr)[-600:])
g = open(f"{S}/cal/plate_1.gcode").read()
i = g.index("\n", g.index("\nPRINT_START") + 1) + 1
g = (g[:i] + f"SET_VELOCITY_LIMIT VELOCITY={TOP}\n"
     f"TUNING_TOWER COMMAND=M220 PARAMETER=S START=100 STEP_DELTA={100 * 10 / BASE:.4f} STEP_HEIGHT={BAND}\n" + g[i:])
open(f"{S}/cal/ad4_cal_flow_tower.gcode", "w").write(g)
print(f"{H} mm tall, bands {list(range(BASE, TOP + 1, 10))} mm/s")
