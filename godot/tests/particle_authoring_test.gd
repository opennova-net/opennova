extends GutTest

# Authoring + validity + round-trip coverage for the particle editor model.
# These exercise the document model (ParticleEditor), the C++ canonical-table
# bindings, and writer/parser round-trips directly (no renderer / workstation),
# so they stay deterministic and isolated from the flaky shared-viewport suite.

const ParticleEditorScript = preload("res://modtools/particle/particle_editor.gd")
const ParticleBlueprintScreenScript = preload("res://modtools/particle/blueprint/particle_blueprint_screen.gd")
const ParticleDefInspectorScript = preload("res://modtools/particle/inspectors/particle_inspector.gd")
const ParticleTableInspectorScene = preload("res://modtools/particle/inspectors/table_inspector.tscn")

const OUTPUT_DIR_NAME := "particle_authoring_test"
const PARTICLE_FLAG_FOREVER_EMIT := NovaParticleDef.FLAG_FOREVER_EMIT
const RESOURCE_FIXTURE := "res://tests/particle_resource_smoke.ptl"
const USER_RESOURCE_PATH := "user://particle_authoring_test/resource_saver_roundtrip.ptl"


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


# --- ResourceLoader / ResourceSaver virtual-path smoke -----------------------

func test_resource_loader_reads_res_path_and_sets_logical_paths() -> void:
	var loaded := ResourceLoader.load(
			RESOURCE_FIXTURE, "NovaParticleFile", ResourceLoader.CACHE_MODE_IGNORE) as NovaParticleFile
	assert_not_null(loaded, "ResourceLoader should open PTL files through res:// FileAccess.")
	if loaded == null:
		return
	assert_not_null(loaded.find_particle("ResourceLoaderSmoke"), "The packed-compatible fixture should parse.")
	assert_eq(String(loaded.source_path), RESOURCE_FIXTURE,
			"The document source path should retain the logical resource path.")
	assert_eq(String(loaded.resource_path), RESOURCE_FIXTURE,
			"The Godot Resource path should match the logical resource path.")


func test_resource_saver_round_trips_user_path() -> void:
	var file := NovaParticleFile.new()
	var particle := NovaParticleDef.new()
	particle.id = "UserRoundTrip"
	particle.emit_burst = 1
	_append(file, "particles", particle)

	var save_error := ResourceSaver.save(file, USER_RESOURCE_PATH)
	assert_eq(save_error, OK,
			"ResourceSaver should write PTL files through user:// FileAccess.")
	if save_error != OK:
		return
	assert_eq(String(file.source_path), USER_RESOURCE_PATH,
			"Saving should adopt the logical user:// source path.")

	var reloaded := ResourceLoader.load(
			USER_RESOURCE_PATH, "NovaParticleFile", ResourceLoader.CACHE_MODE_IGNORE) as NovaParticleFile
	assert_not_null(reloaded, "ResourceLoader should reopen the user:// PTL file.")
	if reloaded == null:
		return
	assert_not_null(reloaded.find_particle("UserRoundTrip"), "Saved particle data should round-trip.")
	assert_eq(String(reloaded.source_path), USER_RESOURCE_PATH)
	assert_eq(String(reloaded.resource_path), USER_RESOURCE_PATH)


func test_unknown_particle_keys_survive_godot_load_save_in_order() -> void:
	var source := """[particledef]
{
	id = UnknownKeyProbe;
	future_mode = first;
	future_mode = second;
	g1_future_mode = layer_first;
	g1_future_mode = layer_second;
}
"""
	var file := NovaParticleFile.new()
	assert_eq(file.load_from_buffer(source.to_utf8_buffer(), "memory.ptl"), OK)
	var particle := file.find_particle("UnknownKeyProbe")
	assert_not_null(particle)
	var original: Array = particle.unknown_keys
	assert_eq(original.size(), 4, "duplicate unknown keys are retained")
	assert_eq(String(original[0].value), "first")
	assert_eq(String(original[1].value), "second")
	assert_eq(String(original[2].key), "g1_future_mode")
	assert_eq(String(original[3].value), "layer_second")

	var path := _output_dir().path_join("unknown_keys.ptl")
	assert_eq(file.save_to_file(path), OK)
	var reloaded := NovaParticleFile.new()
	assert_eq(reloaded.load_from_file(path), OK)
	var round_tripped: Array = reloaded.find_particle("UnknownKeyProbe").unknown_keys
	assert_eq(round_tripped.size(), 4)
	assert_eq(String(round_tripped[0].key), "future_mode")
	assert_eq(String(round_tripped[0].value), "first")
	assert_eq(String(round_tripped[1].value), "second")
	assert_eq(String(round_tripped[2].key), "g1_future_mode")
	assert_eq(String(round_tripped[2].value), "layer_first")
	assert_eq(String(round_tripped[3].value), "layer_second")


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


func test_sparse_retail_graphic_slots_are_valid() -> void:
	var editor = ParticleEditorScript.new()
	var particle = editor.add_particle()
	var graphics: Array = particle.get_graphics()
	graphics[0].present = false
	graphics[1].present = true
	graphics[1].index = 2
	particle.set_graphics(graphics)

	assert_false(_has_severity(editor.validate(), "error"),
			"A retail PTL may author graphic2 without graphic1 and must remain saveable.")


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
	var child = editor.add_particle()
	var e = editor.add_effect()
	editor.effect_add_pdef(e, p.id)
	assert_true(e.pdefs.has(p.id), "effect_add_pdef adds the particle to the effect's pdefs.")
	editor.effect_add_pdef(e, p.id)
	assert_eq(e.pdefs.count(p.id), 1, "Adding the same pdef twice should not duplicate it.")

	editor.set_child_id(p, child.id)
	assert_eq(String(p.child_id), child.id,
			"set_child_id wires a particledef to another particledef.")

	editor.effect_remove_pdef(e, p.id)
	assert_false(e.pdefs.has(p.id), "effect_remove_pdef removes the membership.")


func test_particle_rename_preserves_same_file_references() -> void:
	var editor = ParticleEditorScript.new()
	var target = editor.add_particle()
	var parent = editor.add_particle()
	var effect = editor.add_effect()
	editor.effect_add_pdef(effect, target.id)
	editor.set_child_id(parent, target.id)

	editor.set_particle_id(target, "RenamedParticle")

	assert_eq(target.id, "RenamedParticle")
	assert_eq(String(effect.pdefs[0]), "RenamedParticle",
			"Renaming a local particle keeps effect membership attached.")
	assert_eq(String(parent.child_id), "RenamedParticle",
			"Renaming a local particle keeps child spawn chains attached.")


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
	# Table lookups fold case like the engine's _stricmp resolve
	# [orig: table find @ 0x5e9540]; shipped data mixes cases
	# (ambfx.ptl green_func = Table11Alt vs id = table11Alt).
	var folded: NovaParticleTable = reloaded.find_table(t.id.to_upper())
	assert_not_null(folded, "find_table must resolve case-insensitively.")


func test_table_rename_preserves_curve_and_handle_references() -> void:
	var editor = ParticleEditorScript.new()
	var table = editor.add_table()
	var particle = editor.add_particle()
	editor.assign_curve(particle, "scale_func", table.id)
	var graphics: Array = particle.get_graphics()
	graphics[0].get_alpha_func().name = table.id
	graphics[0].get_alpha_func().present = true
	particle.set_graphics(graphics)
	var handles := NovaParticleTableHandles.new()
	handles.table_id = table.id
	_append(editor.particle_file, "table_handles", handles)

	editor.set_table_id(table, "RenamedCurve")

	assert_eq(table.id, "RenamedCurve")
	assert_eq(particle.get_scale_func().name, "RenamedCurve",
			"Particle-level curve assignments follow a local table rename.")
	assert_eq(graphics[0].get_alpha_func().name, "RenamedCurve",
			"Graphic-layer curve assignments follow a local table rename.")
	assert_eq(handles.table_id, "RenamedCurve",
			"Editor handle metadata follows its table rename.")


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
	assert_not_null(graph, "The blueprint screen should hold a GraphEdit.")
	var node_count := 0
	for child in graph.get_children():
		if child is GraphNode:
			node_count += 1
	assert_eq(node_count, 4, "The graph should show one node per effect/particle/table (2+1+1).")


func test_blueprint_child_connection_targets_a_particle_not_an_effect() -> void:
	var editor = ParticleEditorScript.new()
	var parent = editor.add_particle()
	var child = editor.add_particle()
	var effect = editor.add_effect()
	var screen = add_child_autofree(ParticleBlueprintScreenScript.new())
	await get_tree().process_frame
	screen.set_workspace(editor)
	screen.set_particle_editor(editor)
	await get_tree().process_frame

	var parent_node: StringName = screen._obj_to_name[parent]
	var child_node: StringName = screen._obj_to_name[child]
	var effect_node: StringName = screen._obj_to_name[effect]
	screen._on_connection_request(parent_node, 0, child_node, 1)
	assert_eq(String(parent.child_id), child.id,
			"particle output connects to the child-particle input")
	screen._on_connection_request(parent_node, 0, effect_node, 0)
	assert_eq(String(parent.child_id), child.id,
			"an effect node can never replace a particledef child_id")


func test_effect_membership_edit_rebuilds_graph_and_live_preview() -> void:
	var editor = ParticleEditorScript.new()
	var first = editor.add_particle()
	var second = editor.add_particle()
	var effect = editor.add_effect()
	editor.set_effect_pdefs(effect, PackedStringArray([first.id]))

	var screen = add_child_autofree(ParticleBlueprintScreenScript.new())
	var preview := ParticlePreview.new()
	add_child_autofree(preview)
	await get_tree().process_frame
	screen.set_workspace(editor)
	screen.set_particle_editor(editor)
	preview.set_particle_file(editor.particle_file)
	preview.set_effect(effect)
	editor.preview_refresh_requested.connect(preview.request_live_refresh)
	assert_eq(preview.get_emitter_count(), 1)

	editor.set_effect_pdefs(effect, PackedStringArray([first.id, second.id]))
	var graph := _find_graphedit(screen)
	assert_eq(graph.get_connection_list().size(), 2,
			"document_changed immediately rebuilds membership edges")
	await get_tree().create_timer(0.2).timeout
	assert_eq(preview.get_emitter_count(), 2,
			"preview_refresh_requested rebuilds the selected effect emitters")


func test_blueprint_highlights_only_active_workflow_selection() -> void:
	var workspace := ParticleEditorWorkspace.new()
	workspace.particle_editor.add_particle()
	workspace.particle_editor.add_effect()
	workspace.particle_editor.add_table()
	var screen = add_child_autofree(ParticleBlueprintScreenScript.new())
	await get_tree().process_frame
	screen.set_workspace(workspace)
	screen.set_particle_editor(workspace.particle_editor)
	await get_tree().process_frame
	assert_eq(_selected_graph_node_count(_find_graphedit(screen)), 1,
			"Particles workflow highlights only its current particle")
	workspace.select_workflow(ParticleEditorWorkspace.Workflow.EFFECTS)
	screen._highlight_selected()
	assert_eq(_selected_graph_node_count(_find_graphedit(screen)), 1,
			"Effects workflow highlights only its current effect")


func test_particle_inspector_rename_refreshes_graph_without_rebuilding_its_form() -> void:
	var editor = ParticleEditorScript.new()
	var particle = editor.add_particle()
	var screen = add_child_autofree(ParticleBlueprintScreenScript.new())
	var inspector = add_child_autofree(ParticleDefInspectorScript.new())
	await get_tree().process_frame
	screen.set_particle_editor(editor)
	inspector.set_particle_editor(editor)
	var id_edit := _find_line_edit_with_text(inspector, particle.id)
	assert_not_null(id_edit)

	id_edit.text = "RenamedFromInspector"
	id_edit.text_changed.emit(id_edit.text)

	assert_eq(particle.id, "RenamedFromInspector",
			"The visible name field commits through the particle model.")
	assert_true(is_instance_valid(id_edit),
			"A live text callback must not free and rebuild its own inspector form.")
	assert_true(_graph_has_title(_find_graphedit(screen), "RenamedFromInspector"),
			"The blueprint title refreshes from the particle mutation signal.")


func test_particle_inspector_child_edit_refreshes_spawn_edge_in_place() -> void:
	var editor = ParticleEditorScript.new()
	var parent = editor.add_particle()
	var child = editor.add_particle()
	editor.select_particle(parent)
	var screen = add_child_autofree(ParticleBlueprintScreenScript.new())
	var inspector = add_child_autofree(ParticleDefInspectorScript.new())
	await get_tree().process_frame
	screen.set_particle_editor(editor)
	inspector.set_particle_editor(editor)
	var child_edit := _find_labeled_line_edit(inspector, "Spawns on death")
	assert_not_null(child_edit)

	child_edit.text = child.id
	child_edit.text_submitted.emit(child_edit.text)

	assert_eq(parent.child_id, child.id)
	assert_true(is_instance_valid(child_edit),
			"Editing a spawn reference must not rebuild its active form.")
	assert_eq(_find_graphedit(screen).get_connection_list().size(), 1,
			"The blueprint spawn edge refreshes from the child-id mutation.")


func test_particle_field_edit_refreshes_blueprint_summary_in_place() -> void:
	var editor = ParticleEditorScript.new()
	var particle = editor.add_particle()
	var screen = add_child_autofree(ParticleBlueprintScreenScript.new())
	var inspector = add_child_autofree(ParticleDefInspectorScript.new())
	await get_tree().process_frame
	screen.set_particle_editor(editor)
	inspector.set_particle_editor(editor)
	var rate_spin := _find_labeled_spin(inspector, "Emit rate (/s)")
	assert_not_null(rate_spin)

	rate_spin.value = 77.0
	rate_spin.value_changed.emit(77.0)

	assert_almost_eq(particle.emit_rate, 77.0, 0.001)
	assert_true(_graph_has_label(_find_graphedit(screen), "rate 77"),
			"Particle card summaries refresh without rebuilding the inspector.")


func test_table_inspector_rename_keeps_assignments_and_graph_title() -> void:
	var editor = ParticleEditorScript.new()
	var table = editor.add_table()
	var particle = editor.add_particle()
	editor.assign_curve(particle, "scale_func", table.id)
	var screen = add_child_autofree(ParticleBlueprintScreenScript.new())
	var inspector = add_child_autofree(ParticleTableInspectorScene.instantiate())
	await get_tree().process_frame
	screen.set_particle_editor(editor)
	inspector.set_particle_editor(editor)
	var id_edit := _find_labeled_line_edit(inspector, "Name")
	assert_not_null(id_edit, "The table inspector exposes the table identifier.")
	if id_edit == null:
		return

	id_edit.text = "AuthoredCurve"
	id_edit.text_changed.emit(id_edit.text)

	assert_eq(table.id, "AuthoredCurve")
	assert_eq(particle.get_scale_func().name, "AuthoredCurve")
	assert_true(is_instance_valid(id_edit))
	assert_true(_graph_has_title(_find_graphedit(screen), "AuthoredCurve"))


func test_blueprint_multi_delete_removes_the_original_selected_objects() -> void:
	var editor = ParticleEditorScript.new()
	var particle = editor.add_particle()
	var first = editor.add_effect()
	var second = editor.add_effect()
	var screen = add_child_autofree(ParticleBlueprintScreenScript.new())
	await get_tree().process_frame
	screen.set_workspace(editor)
	screen.set_particle_editor(editor)
	var graph := _find_graphedit(screen)
	var first_name := _graph_node_name_with_title(graph, first.id)
	var second_name := _graph_node_name_with_title(graph, second.id)

	graph.delete_nodes_request.emit([first_name, second_name])

	assert_eq(editor.effect_count(), 0,
			"A batched delete removes both originally selected effects.")
	assert_eq(editor.particle_count(), 1)
	assert_eq(editor.particle_file.particles[0], particle,
			"Graph reindexing during deletion must not redirect the second delete.")


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


func _selected_graph_node_count(graph: GraphEdit) -> int:
	var count := 0
	for child in graph.get_children():
		if child is GraphNode and child.selected:
			count += 1
	return count


func _find_line_edit_with_text(root: Node, text: String) -> LineEdit:
	if root is LineEdit and root.text == text:
		return root
	for child in root.get_children():
		var found := _find_line_edit_with_text(child, text)
		if found != null:
			return found
	return null


func _graph_has_title(graph: GraphEdit, title_fragment: String) -> bool:
	for child in graph.get_children():
		if child is GraphNode and title_fragment in child.title:
			return true
	return false


func _find_labeled_line_edit(root: Node, label_text: String) -> LineEdit:
	for child in root.get_children():
		if child is Label and child.text == label_text:
			for sibling in root.get_children():
				if sibling is LineEdit:
					return sibling
		var found := _find_labeled_line_edit(child, label_text)
		if found != null:
			return found
	return null


func _find_labeled_spin(root: Node, label_text: String) -> SpinBox:
	for child in root.get_children():
		if child is Label and child.text == label_text:
			for sibling in root.get_children():
				if sibling is SpinBox:
					return sibling
		var found := _find_labeled_spin(child, label_text)
		if found != null:
			return found
	return null


func _graph_has_label(graph: GraphEdit, text_fragment: String) -> bool:
	for child in graph.get_children():
		if child is GraphNode:
			for content in child.get_children():
				if content is Label and text_fragment in content.text:
					return true
	return false


func _graph_node_name_with_title(graph: GraphEdit, title_fragment: String) -> StringName:
	for child in graph.get_children():
		if child is GraphNode and title_fragment in child.title:
			return child.name
	return &""


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
