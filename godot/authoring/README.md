# godot/authoring/ - the sources of the models we ship

One folder per model: `<name>/<name>.tscn` (the scene), `<name>/<name>.tres`
(its `ModelAuthoringManifest`), the PNG texture sources and any DCC mesh
source (`.glb`, `.blend`) it was imported from. Binaries ride Git LFS. The
exported `.3di` and `.tga` artifacts land flat in `assets/` and are
byte-guarded by `res://tests/model_export_guard_test.gd`; rebuild them with
**OpenNova: Export all authored models** or the headless `--export-models`
command (`godot/addons/opennova_model/README.md`).

A rig's clips live beside it (ADR 0047): the scene's `AnimationPlayer` holds
the Animations, `<name>/<name>_clips.tres` (a `ClipSetSource`) names the
clips they export as, their root motion and triggers, and the `.adm` rows.
The rifle's clips ride a separate 46-row rig scene (`akm/fp_rig.tscn`, the
rows the rifle's parts and the arms' bones both read). The exported `.bad`
files and the `.adm` land in `assets/`, byte-guarded by
`res://tests/clip_export_guard_test.gd`; rebuild with **OpenNova: Export all
authored clips** or the headless `--export-clips` command.

Everything here is original work. A projection of retail bytes may be
inspected in the editor but never saved here or exported (`assets/README.md`).
This tree is excluded from the game export preset.
