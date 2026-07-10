extends GutTest

# Authoring + validity + round-trip coverage for the particle editor model.
# These exercise the document model (ParticleEditor), the C++ canonical-table
# bindings, and writer/parser round-trips directly (no renderer / workstation),
# so they stay deterministic and isolated from the flaky shared-viewport suite.

const ParticleEditorScript = preload("res://modtools/particle/particle_editor.gd")
const ParticleBlueprintScreenScript = preload("res://modtools/particle/blueprint/particle_blueprint_screen.gd")

const OUTPUT_DIR_NAME := "particle_authoring_test"
const PARTICLE_FLAG_FOREVER_EMIT := 1 << 18


func before_each() -> void:
	_cleanup_dir(_output_dir())
	DirAccess.make_dir_recursive_absolute(_output_dir())


func after_each() -> void:
	_cleanup_dir(_output_dir())


# --- Canonical-table bindings -------------------------------------------------

func test_flag_tables_bound_from_cpp() -> void:
	var flags := NovaParticleDef.get_particle_flag_table()
	assert_eq(flags.size(), 29, "Particle flag table should expose all 29 engine entries.")
	assert_true(flags.has("FOREVEREMIT"), "Flag table should include FOREVEREMIT.")
	assert_eq(int(flags["FOREVEREMIT"]), PARTICLE_FLAG_FOREVER_EMIT, "FOREVEREMIT bit must match the engine table.")
	assert_true(flags.has("HAZE") and flags.has("BELOWH20") and flags.has("ABOVEH20"),
			"Flag table should include the water/haze flags added by the cross-witness grill.")

	var moves := NovaParticleDef.get_move_flag_table()
	assert_eq(moves.size(), 5, "Move table should expose all 5 entries.")
	assert_eq(int(moves["ORBIT"]), 1 << 2, "ORBIT must live at bit 2 per the engine table reorder.")

	var blends := NovaParticleDef.get_blend_mode_names()
	assert_eq(blends.size(), 8, "There should be 8 blend modes.")
	assert_eq(String(blends[0]), "blend")
	assert_eq(String(blends[1]), "additive")

	assert_eq(String(NovaParticleDef.format_particle_flags(PARTICLE_FLAG_FOREVER_EMIT)).strip_edges(),
			"FOREVEREMIT", "format_particle_flags should round-trip a single bit to its name.")


# --- Flag bitfield round-trip (the bug fix) ----------------------------------

func test_flag_bitfield_survives_save_and_reload() -> void:
	var file := NovaParticleFile.new()
	var p := NovaParticleDef.new()
	p.id = "Flagged"
	p.flags = PARTICLE_FLAG_FOREVER_EMIT
	p.emit_burst = 1
	_append(file, "particles", p)

	var reloaded := _roundtrip(file)
	var rp: NovaParticleDef = reloaded.find_particle("Flagged")
	assert_not_null(rp, "Saved particle should reload.")
	assert_eq(int(rp.flags) & PARTICLE_FLAG_FOREVER_EMIT, PARTICLE_FLAG_FOREVER_EMIT,
			"A flag toggled via the bitfield must persist through the writer (which serializes flags, not flags_raw).")


# --- Add / duplicate / remove ------------------------------------------------

func test_new_document_add_particle_round_trips() -> void:
	var editor = ParticleEditorScript.new()
	assert_eq(editor.particle_count(), 0, "A new document starts empty.")
	var p = editor.add_particle()
	assert_not_null(p, "add_particle should create a particle.")
	assert_eq(editor.particle_count(), 1)
	assert_true(editor.is_dirty, "Adding a particle marks the document dirty.")
	assert_eq(editor.present_graphic_count(p), 1, "A fresh particle has one visible graphic layer.")

	var path := _output_dir().path_join("added.ptl")
	assert_eq(editor.save_to_path(path), OK, "The new document should save.")
	var reloaded := NovaParticleFile.new()
	assert_eq(reloaded.load_from_file(path), OK, "The saved file should reload as valid PTL.")
	assert_eq(reloaded.particles.size(), 1, "The added particle should be present after reload.")


func test_duplicate_particle_is_a_deep_copy() -> void:
	var editor = ParticleEditorScript.new()
	var p = editor.add_particle()
	p.gravity = 123.0
	p.color1 = Color(0.5, 0.25, 0.1)
	var dup = editor.duplicate_particle(p)
	assert_not_null(dup)
	assert_ne(dup.id, p.id, "A duplicate gets a unique id.")
	assert_almost_eq(dup.gravity, 123.0, 0.001, "Duplicate copies field values.")
	dup.gravity = 5.0
	assert_almost_eq(p.gravity, 123.0, 0.001, "Mutating the duplicate must not touch the original (deep copy).")


func test_remove_particle_updates_selection() -> void:
	var editor = ParticleEditorScript.new()
	var a = editor.add_particle()
	var b = editor.add_particle()
	editor.select_particle(a)
	editor.remove_particle(a)
	assert_eq(editor.particle_count(), 1)
	assert_eq(editor.current_particle, b, "Removing the selected particle re-selects a remaining one.")


# --- Graphic-layer contiguity (valid-PTL invariant) --------------------------

func test_graphic_layers_stay_contiguous_after_removal() -> void:
	var editor = ParticleEditorScript.new()
	var p = editor.add_particle()
	editor.add_graphic_layer(p)
	editor.add_graphic_layer(p)
	assert_eq(editor.present_graphic_count(p), 3, "Three layers should be present.")

	editor.remove_graphic_layer(p, 1)  # remove the middle layer
	assert_eq(editor.present_graphic_count(p), 2, "Two layers remain.")
	var graphics: Array = p.get_graphics()
	assert_true(graphics[0].present, "Slot 0 stays present.")
	assert_true(graphics[1].present, "Remaining layer compacts to slot 1.")
	assert_false(graphics[2].present, "Slot 2 is now empty.")
	assert_false(graphics[3].present, "Slot 3 is empty.")
	assert_eq(graphics.size(), 4, "The graphics array always keeps 4 slots.")

	var issues: Array = editor.validate()
	assert_false(_has_severity(issues, "error"), "Contiguous layers must not raise a validation error.")


# --- Validity gate -----------------------------------------------------------

func test_validate_flags_empty_and_duplicate_ids() -> void:
	var editor = ParticleEditorScript.new()
	var a = editor.add_particle()
	a.id = ""
	var empty_issues: Array = editor.validate()
	assert_true(_has_severity(empty_issues, "error"), "An empty id should be a blocking error.")

	a.id = "dup"
	var b = editor.add_particle()
	b.id = "dup"
	var dup_issues: Array = editor.validate()
	assert_false(_has_severity(dup_issues, "error"), "Duplicate ids are not a hard error.")
	assert_true(_has_severity(dup_issues, "warning"), "Duplicate ids should warn.")


# --- Reference edits (blueprint wiring goes through these) --------------------

func test_effect_and_child_reference_edits() -> void:
	var editor = ParticleEditorScript.new()
	var p = editor.add_particle()
	var e = editor.add_effect()
	editor.effect_add_pdef(e, p.id)
	assert_true(e.pdefs.has(p.id), "effect_add_pdef adds the particle to the effect's pdefs.")
	editor.effect_add_pdef(e, p.id)
	assert_eq(e.pdefs.count(p.id), 1, "Adding the same pdef twice should not duplicate it.")

	editor.set_child_id(p, e.id)
	assert_eq(String(p.child_id), e.id, "set_child_id wires the spawn chain.")

	editor.effect_remove_pdef(e, p.id)
	assert_false(e.pdefs.has(p.id), "effect_remove_pdef removes the membership.")


# --- Curve table editing round-trip ------------------------------------------

func test_table_edit_round_trips() -> void:
	var editor = ParticleEditorScript.new()
	var t = editor.add_table()
	var data: PackedByteArray = t.get_data()
	assert_eq(data.size(), 256, "A table is a 256-byte LUT.")
	data[0] = 200
	data[255] = 33
	t.set_data(data)

	var path := _output_dir().path_join("table.ptl")
	assert_eq(editor.save_to_path(path), OK)
	var reloaded := NovaParticleFile.new()
	assert_eq(reloaded.load_from_file(path), OK)
	var rt: NovaParticleTable = reloaded.find_table(t.id)
	assert_not_null(rt, "The table should reload.")
	var rdata: PackedByteArray = rt.get_data()
	assert_eq(int(rdata[0]), 200, "Edited curve bytes must survive a save/reload.")
	assert_eq(int(rdata[255]), 33)


# --- Blueprint graph builds from the model -----------------------------------

func test_blueprint_builds_nodes_from_model() -> void:
	var editor = ParticleEditorScript.new()
	editor.add_particle()
	editor.add_particle()
	editor.add_effect()
	editor.add_table()

	var screen = add_child_autofree(ParticleBlueprintScreenScript.new())
	await get_tree().process_frame
	screen.set_workspace(null)
	screen.set_particle_editor(editor)
	await get_tree().process_frame

	var graph := _find_graphedit(screen)
	assert_not_null(graph, "The blueprint screen should host a GraphEdit.")
	var node_count := 0
	for child in graph.get_children():
		if child is GraphNode:
			node_count += 1
	assert_eq(node_count, 4, "The graph should show one node per effect/particle/table (2+1+1).")


# --- Helpers -----------------------------------------------------------------

func _roundtrip(file: NovaParticleFile) -> NovaParticleFile:
	var path := _output_dir().path_join("roundtrip.ptl")
	assert_eq(file.save_to_file(path), OK, "File should save.")
	var reloaded := NovaParticleFile.new()
	assert_eq(reloaded.load_from_file(path), OK, "File should reload.")
	return reloaded


func _append(file: NovaParticleFile, prop: String, value) -> void:
	var arr: Array = file.get(prop)
	arr.append(value)
	file.set(prop, arr)


func _has_severity(issues: Array, severity: String) -> bool:
	for issue in issues:
		if issue.get("severity") == severity:
			return true
	return false


func _find_graphedit(root: Node) -> GraphEdit:
	if root is GraphEdit:
		return root
	for child in root.get_children():
		var found := _find_graphedit(child)
		if found != null:
			return found
	return null


func _output_dir() -> String:
	return OS.get_user_data_dir().path_join(OUTPUT_DIR_NAME)


func _cleanup_dir(path: String) -> void:
	if not DirAccess.dir_exists_absolute(path):
		return
	for file in DirAccess.get_files_at(path):
		DirAccess.remove_absolute(path.path_join(file))
	for dir in DirAccess.get_directories_at(path):
		_cleanup_dir(path.path_join(dir))
	DirAccess.remove_absolute(path)
