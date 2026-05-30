extends SceneTree

# Headless regression check for the "save project → reopen → textures gone"
# bug that used to happen when the Godot editor wrote a .tpj sidecar and
# NovaTerrainData::load() hard-required a CPT.
#
# Now that CPT is optional and the editor saves .trn-only projects, this
# probe confirms the whole round-trip:
#   1. NovaTerrainData.load(Dvxi5.trn) — import from res://
#   2. ResourceSaver.save(.trn) into a fresh dir (polydata empty)
#   3. ResourceLoader.load(.trn) back from that dir
#   4. Assert texture filenames round-trip and each file exists on disk.
#
# Run: godot --headless --path godot -s res://tests/trn_project_roundtrip_probe.gd


func _initialize() -> void:
	call_deferred("_run")


func _run() -> void:
	var source_trn := ProjectSettings.globalize_path("res://../fixtures/godot/dvxi5/Dvxi5.trn")
	var output_dir := OS.get_user_data_dir() + "/trn_project_roundtrip_probe"
	_cleanup_dir(output_dir)
	DirAccess.make_dir_recursive_absolute(output_dir)
	print("[probe] work dir: ", output_dir)

	print("\n[step 1] import Dvxi5.trn")
	var imported := NovaTerrainData.new()
	imported.set_trn_path(source_trn)
	if imported.load() != OK:
		push_error("load(Dvxi5.trn) failed")
		quit(1); return
	if not imported.is_loaded():
		push_error("imported data reports loaded=false")
		quit(1); return

	print("\n[step 2] save as project (no CPT, no .tpj)")
	var terrain_name := "Dvxi5"
	var trn_dir := ProjectSettings.globalize_path("res://../fixtures/godot/dvxi5")
	var pcx_slots := {"charmap": "_m.pcx", "foliagemap": "_f.pcx"}
	for slot_id in pcx_slots.keys():
		var slot_suffix := String(pcx_slots[slot_id])
		var out_path := output_dir + "/" + terrain_name + slot_suffix
		var err := imported.save_pcx_slot(String(slot_id), out_path)
		if err != OK:
			push_error("save_pcx_slot(" + String(slot_id) + ") err=" + str(err))
			quit(1); return
	var tga_slots := {
		"colormap": "Dvxi5_c.tga",
		"detailmap": "Dvxi5_dm.tga",
		"detailmap_c1": "Dvxi5_dc1.tga",
		"detailmap_c2": "Dvxi5_dc2.tga",
		"detailmap_c3": "Dvxi5_dc3.tga",
		"detailmap2": "Dvxi5_dm2.tga",
		"detailmapdist": "Dvxi5_dmd.tga",
		"detailblendmap": "Dvxi5_d1.tga",
		"tilestrip": "Dvxi5_t.tga",
	}
	for slot_id in tga_slots.keys():
		var export_filename := String(tga_slots[slot_id])
		var original := String(imported.get_trn_texture_filename(String(slot_id)))
		var src := trn_dir + "/" + original
		if original.is_empty() or not FileAccess.file_exists(src):
			src = ""
			if slot_id == "tilestrip":
				src = trn_dir + "/TRNTILE10.TGA"
		if not src.is_empty() and FileAccess.file_exists(src):
			var bytes := FileAccess.get_file_as_bytes(src)
			var f := FileAccess.open(output_dir + "/" + export_filename, FileAccess.WRITE)
			if f:
				f.store_buffer(bytes)
				f.close()

	# Mirror prepare_data_for_trn_save(name, "") — save leaves polydata empty.
	imported.set_polydata_filename("")
	imported.set_trn_texture_filename("colormap", terrain_name + "_c.tga")
	imported.set_trn_texture_filename("detailblendmap", terrain_name + "_d1.tga")
	imported.set_trn_texture_filename("charmap", terrain_name + "_m.pcx")
	imported.set_trn_texture_filename("foliagemap", terrain_name + "_f.pcx")
	imported.set_trn_texture_filename("tilestrip", terrain_name + "_t.tga")
	imported.set_terrain_name(terrain_name)

	var trn_path := output_dir + "/" + terrain_name + ".trn"
	var save_err := ResourceSaver.save(imported, trn_path)
	if save_err != OK:
		push_error("ResourceSaver.save(.trn) err=" + str(save_err))
		quit(1); return

	# There must be no .tpj file written.
	if FileAccess.file_exists(output_dir + "/" + terrain_name + ".tpj"):
		push_error("probe FAIL: save produced a .tpj — Save Project is supposed to be .trn-only now")
		quit(1); return

	print("\n[step 3] reopen .trn (CPT absent — must not block)")
	var reopened := ResourceLoader.load(trn_path, "NovaTerrainData", ResourceLoader.CACHE_MODE_IGNORE) as NovaTerrainData
	if reopened == null:
		push_error("probe FAIL: ResourceLoader.load(.trn) returned null — NovaTerrainData::load() should tolerate missing CPT")
		quit(1); return
	if not reopened.is_loaded():
		push_error("probe FAIL: reopened data reports loaded=false")
		quit(1); return

	print("\n[step 4] verify texture filename round-trip")
	var failures: Array[String] = []
	var expected := {
		"colormap": "Dvxi5_c.tga",
		"charmap": "Dvxi5_m.pcx",
		"foliagemap": "Dvxi5_f.pcx",
		"tilestrip": "Dvxi5_t.tga",
		"detailblendmap": "Dvxi5_d1.tga",
	}
	for slot_id in expected.keys():
		var want := String(expected[slot_id])
		var got := String(reopened.get_trn_texture_filename(String(slot_id)))
		if got != want:
			failures.append("%s: trn reads '%s' expected '%s'" % [slot_id, got, want])
			continue
		if not FileAccess.file_exists(output_dir + "/" + got):
			failures.append("%s: file '%s' missing on disk" % [slot_id, got])

	if not failures.is_empty():
		for f in failures:
			push_error("probe FAIL (baseline): " + f)
		quit(1); return

	# -----------------------------------------------------------------
	# Rename round-trip: Dvxi5 → MyMap. Verify the renamed terrain saves
	# with new-name files and reopens cleanly.
	# -----------------------------------------------------------------
	print("\n[step 5] rename round-trip (Dvxi5 → MyMap)")
	var rename_dir := OS.get_user_data_dir() + "/trn_project_roundtrip_probe_renamed"
	_cleanup_dir(rename_dir)
	DirAccess.make_dir_recursive_absolute(rename_dir)

	var renamed := NovaTerrainData.new()
	renamed.set_trn_path(source_trn)
	if renamed.load() != OK:
		push_error("second load(Dvxi5.trn) failed")
		quit(1); return

	var new_name := "MyMap"
	renamed.set_terrain_name(new_name)

	for slot_id in pcx_slots.keys():
		var slot_suffix := String(pcx_slots[slot_id])
		if renamed.save_pcx_slot(String(slot_id), rename_dir + "/" + new_name + slot_suffix) != OK:
			push_error("renamed save_pcx_slot " + String(slot_id) + " failed")
			quit(1); return
	for slot_id in tga_slots.keys():
		var export_filename := String(tga_slots[slot_id]).replace("Dvxi5", new_name)
		var original := String(renamed.get_trn_texture_filename(String(slot_id)))
		var src := trn_dir + "/" + original
		if original.is_empty() or not FileAccess.file_exists(src):
			src = ""
			if slot_id == "tilestrip":
				src = trn_dir + "/TRNTILE10.TGA"
		if not src.is_empty() and FileAccess.file_exists(src):
			var bytes := FileAccess.get_file_as_bytes(src)
			var f := FileAccess.open(rename_dir + "/" + export_filename, FileAccess.WRITE)
			if f:
				f.store_buffer(bytes)
				f.close()

	renamed.set_polydata_filename("")
	renamed.set_trn_texture_filename("colormap", new_name + "_c.tga")
	renamed.set_trn_texture_filename("detailblendmap", new_name + "_d1.tga")
	renamed.set_trn_texture_filename("charmap", new_name + "_m.pcx")
	renamed.set_trn_texture_filename("foliagemap", new_name + "_f.pcx")
	renamed.set_trn_texture_filename("tilestrip", new_name + "_t.tga")

	var renamed_trn_path := rename_dir + "/" + new_name + ".trn"
	if ResourceSaver.save(renamed, renamed_trn_path) != OK:
		push_error("renamed ResourceSaver.save(.trn) failed")
		quit(1); return

	for expected_file in [new_name + ".trn", new_name + "_m.pcx", new_name + "_f.pcx", new_name + "_t.tga", new_name + "_c.tga", new_name + "_d1.tga"]:
		if not FileAccess.file_exists(rename_dir + "/" + expected_file):
			failures.append("after rename-save, expected file '" + expected_file + "' is missing")

	var reopened_renamed := ResourceLoader.load(renamed_trn_path, "NovaTerrainData", ResourceLoader.CACHE_MODE_IGNORE) as NovaTerrainData
	if reopened_renamed == null:
		push_error("renamed ResourceLoader.load(.trn) returned null")
		quit(1); return
	if String(reopened_renamed.get_terrain_name()) != new_name:
		failures.append("reopened terrain_name=%s expected %s" % [reopened_renamed.get_terrain_name(), new_name])
	var renamed_suffixes := {"colormap": "_c.tga", "charmap": "_m.pcx", "foliagemap": "_f.pcx", "tilestrip": "_t.tga", "detailblendmap": "_d1.tga"}
	for slot_id in renamed_suffixes.keys():
		var want := new_name + String(renamed_suffixes[slot_id])
		var got := String(reopened_renamed.get_trn_texture_filename(String(slot_id)))
		if got != want:
			failures.append("renamed %s: trn reads '%s' expected '%s'" % [slot_id, got, want])

	if failures.is_empty():
		print("trn_project_roundtrip_probe: OK (baseline + rename)")
	else:
		for f in failures:
			push_error("probe FAIL: " + f)
		quit(1); return

	quit(0)


func _cleanup_dir(path: String) -> void:
	if not DirAccess.dir_exists_absolute(path):
		return
	for file in DirAccess.get_files_at(path):
		DirAccess.remove_absolute(path + "/" + file)
	DirAccess.remove_absolute(path)
