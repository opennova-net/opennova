extends GutTest

# C7 consumption badges: fields whose edits don't reach the picture yet are
# labeled (partial / unconsumed glyph + plain-language tooltip) from
# EnvFile.get_field_consumption(), and the previously-hidden fields are
# editable Advanced rows.

const EnvironmentEditorScript = preload("res://modtools/environment/environment_editor.gd")
const EnvironmentInspectorScript = preload("res://modtools/environment/environment_inspector.gd")


func _make() -> Dictionary:
	var editor = add_child_autofree(EnvironmentEditorScript.new())
	editor.create_default_environment(false)
	var inspector = add_child_autofree(EnvironmentInspectorScript.new())
	inspector.set_environment_editor(editor)
	return {"editor": editor, "inspector": inspector}


func _find_label(inspector: Node, text: String) -> Label:
	for label in inspector.find_children("", "Label", true, false):
		if label.text == text:
			return label
	return null


func test_partial_fields_carry_badge_and_tooltip() -> void:
	var ctx := _make()
	var terrain := _find_label(ctx.inspector, "Terrain " + EnvironmentInspectorScript.BADGE_PARTIAL)
	assert_not_null(terrain, "the terrain tint row is badged partial")
	if terrain != null:
		assert_false(terrain.tooltip_text.is_empty(), "the badge explains itself in plain language")


func test_unconsumed_fields_carry_badge() -> void:
	var ctx := _make()
	# Vertex tint is the standing unconsumed row (retail ignores it too).
	# Iris moved to honored at REN-5 (the modulator chain went live,
	# env #17 — docs/render/render-lighting-re.md); it keeps a caveat
	# tooltip like other honored-with-scope rows, not a badge.
	var vertex := _find_label(ctx.inspector, "Vertex tint " + EnvironmentInspectorScript.BADGE_UNCONSUMED)
	assert_not_null(vertex, "the vertex tint row is badged unconsumed")
	var iris := _find_label(ctx.inspector, "Iris percent")
	assert_not_null(iris, "the iris row is honored and unbadged since REN-5")
	if iris != null:
		assert_false(iris.tooltip_text.is_empty(),
			"the iris row keeps its interior-sampling caveat in the tooltip")


func test_honored_fields_carry_no_badge() -> void:
	var ctx := _make()
	# Clouds and Water are both FIELD-WIRED rows whose table status is honored -
	# they must keep a plain label (a pin on an unwired row would stay green even
	# if every status grew a badge).
	var clouds := _find_label(ctx.inspector, "Clouds")
	assert_not_null(clouds, "honored rows keep their plain label")
	assert_null(_find_label(ctx.inspector, "Clouds " + EnvironmentInspectorScript.BADGE_PARTIAL),
		"honored rows are not badged")
	assert_not_null(_find_label(ctx.inspector, "Water"), "water_color is honored and unbadged")
	if clouds != null:
		assert_false(clouds.tooltip_text.is_empty(),
			"honored-with-scope rows still explain their caveat in the tooltip")


func test_advanced_rows_sync_from_env() -> void:
	var ctx := _make()
	var env: EnvFile = ctx.editor.env_file
	env.set_water_murk(0.55)
	env.set_iris_percent(42.0)
	ctx.inspector.sync_from_editor()
	assert_almost_eq(float(ctx.inspector._water_murk.value), 0.55, 0.001, "murk row reads the env value")
	assert_almost_eq(float(ctx.inspector._iris_percent.value), 42.0, 0.001, "iris row reads the env value")


func test_advanced_edit_routes_and_undoes() -> void:
	var ctx := _make()
	var original: float = ctx.editor.env_file.get_water_murk()
	ctx.inspector._water_murk.value = 0.25
	ctx.inspector._water_murk.get_line_edit().focus_exited.emit()
	assert_almost_eq(ctx.editor.env_file.get_water_murk(), 0.25, 0.001, "the edit reaches the env file")
	assert_true(ctx.editor.can_undo(), "the edit is one undo step")
	ctx.editor.undo()
	assert_almost_eq(ctx.editor.env_file.get_water_murk(), original, 0.001, "undo restores the prior murk")
