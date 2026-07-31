extends GutTest

# Variable rename: a display-only friendly name written to the project's
# .music_profile.json sidecar via MusVarNames.set_label. The contract: every
# surface reads the new name (inspector rows, pickers, prettified expressions)
# while serialization keeps emitting the VarXX token -- a rename can never
# change what compiles.

const MusVarNames = preload("res://modtools/music/mus_var_names.gd")
const MusDisplayNames = preload("res://modtools/music/mus_display_names.gd")
const MusExpr = preload("res://modtools/music/mus_expr.gd")
const ExprRowClass = preload("res://modtools/music/ui/expr_row.gd")
const VarInspectorScene = preload("res://modtools/music/ui/var_inspector.tscn")

const PROFILE := "user://music_rename_test.music_profile.json"


func after_each() -> void:
	var abs := ProjectSettings.globalize_path(PROFILE)
	if FileAccess.file_exists(abs):
		DirAccess.remove_absolute(abs)


func test_set_label_round_trips_through_label_for():
	assert_eq(MusVarNames.set_label(PROFILE, "myscript", 5, "Tension"), OK)
	assert_eq(MusVarNames.label_for("myscript", 5, PROFILE), "Tension (Var05)",
		"the profile name reads back with its token suffix")
	assert_eq(MusVarNames.label_for("myscript", 6, PROFILE), "Var06",
		"unnamed vars stay raw")


func test_set_label_merges_and_clears():
	assert_eq(MusVarNames.set_label(PROFILE, "myscript", 5, "Tension"), OK)
	assert_eq(MusVarNames.set_label(PROFILE, "myscript", 7, "Danger"), OK)
	assert_eq(MusVarNames.label_for("myscript", 5, PROFILE), "Tension (Var05)",
		"naming a second var keeps the first")
	# Empty label clears the override.
	assert_eq(MusVarNames.set_label(PROFILE, "myscript", 5, ""), OK)
	assert_eq(MusVarNames.label_for("myscript", 5, PROFILE), "Var05", "cleared name falls back")
	assert_eq(MusVarNames.label_for("myscript", 7, PROFILE), "Danger (Var07)", "other names survive")


func test_set_label_overrides_builtin_known_names():
	assert_eq(MusVarNames.set_label(PROFILE, "gamescript", 1, "Combat"), OK)
	assert_eq(MusVarNames.label_for("gamescript", 1, PROFILE), "Combat (Var01)",
		"a profile name wins over the built-in MissionActive")
	assert_eq(MusVarNames.set_label(PROFILE, "gamescript", 1, ""), OK)
	assert_eq(MusVarNames.label_for("gamescript", 1, PROFILE), "MissionActive (Var01)",
		"clearing falls back to the built-in name")


func test_set_label_requires_a_profile_path():
	assert_ne(MusVarNames.set_label("", "myscript", 5, "Tension"), OK,
		"no sidecar path (unsaved project) refuses the write")


func test_inspector_rename_updates_row_and_announces():
	var vi: Control = VarInspectorScene.instantiate()
	add_child_autofree(vi)
	var director := NovaMusicDirector.new()
	add_child_autofree(director)
	vi.bind_director(director)
	vi.set_script_name("myscript")
	vi.set_profile_path(PROFILE)
	await get_tree().process_frame
	var announced := []
	vi.names_changed.connect(func(): announced.append(1))
	vi._apply_rename(5, "Tension")
	assert_eq(vi.get_row_label_text(5), "Tension (Var05)", "the inspector row shows the new name")
	assert_eq(announced, [1], "names_changed lets the owner refresh its other surfaces")


func test_inspector_rename_disabled_without_profile_path():
	var vi: Control = VarInspectorScene.instantiate()
	add_child_autofree(vi)
	var director := NovaMusicDirector.new()
	add_child_autofree(director)
	vi.bind_director(director)
	vi.set_script_name("myscript")
	await get_tree().process_frame
	# Find the row's rename button (the ✎ inside the name cell).
	var entry: Dictionary = vi._controls.get(5, {})
	assert_false(entry.is_empty(), "row built")
	var name_cell := (entry["name_label"] as Label).get_parent()
	var btn: Button = null
	for c in name_cell.get_children():
		if c is Button:
			btn = c
	assert_not_null(btn, "rename affordance present")
	assert_true(btn.disabled, "disabled when the project has no save path yet")
	assert_string_contains(btn.tooltip_text, "Save the project first")


func test_renamed_var_displays_friendly_but_serializes_token():
	assert_eq(MusVarNames.set_label(PROFILE, "myscript", 5, "Tension"), OK)
	var vars := []
	for i in range(17):
		vars.append({"token": "Var%02d" % i, "label": MusVarNames.label_for("myscript", i, PROFILE)})
	# Display: prettified expressions read the short name.
	assert_eq(MusDisplayNames.pretty_expr("(Var05 > 3)", vars), "(Tension > 3)")
	# Serialization: an expression row built around the renamed var still emits
	# the raw token, so the compiled script is untouched by the rename.
	var r: Control = ExprRowClass.new()
	add_child_autofree(r)
	r.setup(vars, null)
	r.set_expr(MusExpr.binop(">", MusExpr.varref("Var", 5), MusExpr.literal(3)))
	assert_eq(r.get_expr_text(), "(Var05 > 3)", "the canonical text keeps the VarXX token")
