"""Slice Klipper's square_tower.stl for the Q1 PA tower (OrcaSlicer CLI, run from WSL).
Paths are Caleb's ASRock: copy square_tower.stl into S first. Output: S/cal/plate_1.gcode;
then add the TUNING_TOWER lines after PRINT_START (see ../Q1-pressure-advance.md)."""
import glob, json, os, subprocess
P = "/mnt/c/Program Files/OrcaSlicer/resources/profiles"
U = "/mnt/c/Users/caleb/FreeCAD/slicer/OrcaSlicer"
S = "/mnt/c/Users/caleb/AppData/Local/Temp/ad4cube/s1"
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
                    "outer_wall_speed": "60", "inner_wall_speed": "60", "post_process": [],
                    **{k: "0" for k in ("default_acceleration", "outer_wall_acceleration", "inner_wall_acceleration",
                        "internal_solid_infill_acceleration", "sparse_infill_acceleration", "travel_acceleration",
                        "top_surface_acceleration", "initial_layer_acceleration", "bridge_acceleration")}},
        "filament": {"slow_down_for_layer_cooling": ["0"]}}
for kind, name in [("machine", "AD4 0.4 Nozzle (Caleb)"), ("process", "AD4 PLA Standard"), ("filament", "AD4 Rapid PLA+")]:
    j = resolve(name)
    j.update(over.get(kind, {}))
    j["inherits"] = idx[name]["inherits"]
    j["from"] = "User"; j["type"] = kind
    json.dump(j, open(f"{S}/c_{kind}.json", "w"), indent=1)
os.makedirs(S + "/cal", exist_ok=True)
r = subprocess.run(["/mnt/c/Program Files/OrcaSlicer/orca-slicer.exe", "--slice", "0", "--arrange", "1",
    "--load-settings", "c_machine.json;c_process.json", "--load-filaments", "c_filament.json",
    "--outputdir", "cal", "square_tower.stl"], cwd=S, capture_output=True, text=True)
print(r.returncode, (r.stdout + r.stderr)[-600:] if r.returncode else "")
