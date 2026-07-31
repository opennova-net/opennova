extends GutTest

const PageScript := preload(
		"res://engine/debug/pages/debug_rendering_page.gd")
const NovaDebugViewStatus := preload(
		"res://engine/debug/nova_debug_view_status.gd")


class StubViewport:
	extends RefCounted
	var debug_draw := 0


class StubWorld:
	extends RefCounted

	var enabled := {
		&"show_skeletons": false,
		&"show_user_points": false,
		&"show_collision": false,
		&"show_effect_boxes": false,
		&"show_portal_faces": false,
		&"show_round_trails": false,
		&"show_hit_meshes": false,
	}
	var drawable := {&"show_collision": 5}

	func set_skeleton_debug(value: bool) -> void:
		enabled[&"show_skeletons"] = value

	func is_skeleton_debug() -> bool:
		return enabled[&"show_skeletons"]

	func set_user_point_debug(value: bool) -> void:
		enabled[&"show_user_points"] = value

	func is_user_point_debug() -> bool:
		return enabled[&"show_user_points"]

	func set_collision_debug(value: bool) -> void:
		enabled[&"show_collision"] = value

	func is_collision_debug() -> bool:
		return enabled[&"show_collision"]

	func set_particle_debug(value: bool) -> void:
		enabled[&"show_effect_boxes"] = value

	func is_particle_debug() -> bool:
		return enabled[&"show_effect_boxes"]

	func set_occlusion_debug(value: bool) -> void:
		enabled[&"show_portal_faces"] = value

	func is_occlusion_debug() -> bool:
		return enabled[&"show_portal_faces"]

	func set_round_debug(value: bool) -> void:
		enabled[&"show_round_trails"] = value

	func is_round_debug() -> bool:
		return enabled[&"show_round_trails"]

	func set_hitbox_debug(value: bool) -> void:
		enabled[&"show_hit_meshes"] = value

	func is_hitbox_debug() -> bool:
		return enabled[&"show_hit_meshes"]

	func get_debug_view_statuses() -> Array[NovaDebugViewStatus]:
		var result: Array[NovaDebugViewStatus] = []
		for id in enabled:
			var count := int(drawable.get(id, 0))
			result.append(NovaDebugViewStatus.new(
				id,
				bool(enabled[id]),
				bool(enabled[id]),
				count,
				(
						"%d drawable items" % count
						if count > 0
						else "No matching data is currently drawable.")))
		return result


func _make_page() -> Dictionary:
	var viewport := StubViewport.new()
	var world := StubWorld.new()
	var session := NovaDebugSession.new()
	NovaDebugCatalog.install(session)
	NovaDebugCatalog.bind_runtime_targets(
			session,
			Callable(),
			func(): return world,
			Callable(),
			func(): return viewport)
	session.set_presented(true)
	var ctx := NovaDebugContext.new()
	ctx.session = session
	ctx.options = NovaDebugOptionState.new()
	ctx.world_source = func(): return world
	var page: DebugRenderingPage = PageScript.new()
	page.setup(ctx)
	add_child_autofree(page)
	page.refresh()
	return {
		"page": page,
		"session": session,
		"viewport": viewport,
		"world": world,
	}


func test_rendering_uses_real_runtime_overlays_instead_of_dead_godot_hints() -> void:
	var fixture := _make_page()
	var page: DebugRenderingPage = fixture.page

	assert_null(page.find_child("physics_collision_shapes", true, false),
			"gameplay collision is custom, so the empty Godot hint is not offered")
	assert_null(page.find_child("navigation_paths", true, false),
			"the runtime has no Godot navigation data to draw")
	for id in [
		"show_skeletons",
		"show_user_points",
		"show_collision",
		"show_effect_boxes",
		"show_portal_faces",
		"show_round_trails",
		"show_hit_meshes",
	]:
		assert_not_null(page.find_child(id, true, false),
				"%s is a real OpenNova world overlay" % id)
	var effect_boxes := page.find_child(
			"show_effect_boxes", true, false) as CheckBox
	assert_string_contains(effect_boxes.tooltip_text, "Particles catalog issues",
			"missing assets are located where the redesigned UI actually reports them")


func test_rendering_reports_viewport_mode_and_active_overlay_artifacts() -> void:
	var fixture := _make_page()
	var page: DebugRenderingPage = fixture.page
	var session: NovaDebugSession = fixture.session

	assert_eq(session.set_control_value(&"viewport_debug_draw", 3), OK)
	assert_eq(session.set_control_value(&"show_collision", true), OK)
	page.refresh()

	var viewport_state := page.find_child(
			"ViewportDiagnosticState", true, false) as Label
	var overlay_state := page.find_child(
			"WorldOverlayState", true, false) as Label
	assert_not_null(viewport_state)
	assert_not_null(overlay_state)
	if viewport_state != null:
		assert_string_contains(viewport_state.text, "SELECTED")
		assert_string_contains(viewport_state.text, "Overdraw")
		assert_string_contains(viewport_state.text, "brighter",
				"the selected diagnostic explains what visible output means")
		assert_false(viewport_state.text.contains("applied"),
				"selection does not falsely promise that a data-dependent buffer is visible")
	if overlay_state != null:
		assert_string_contains(overlay_state.text, "Collision")
		assert_string_contains(overlay_state.text, "ACTIVE")
		assert_string_contains(overlay_state.text, "5 drawable")

	assert_eq(session.set_control_value(&"viewport_debug_draw", 12), OK)
	page.refresh()
	assert_string_contains(viewport_state.text, "SSAO")
	assert_string_contains(viewport_state.text, "requires SSAO",
			"feature-buffer modes explain why selecting them can show nothing")
