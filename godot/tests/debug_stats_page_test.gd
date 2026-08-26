extends GutTest

# DebugStatsPage: semantic rows and capture lifecycle are exercised through the
# public value interface. The focused layout case observes the rendered Tree at
# the overlay's narrow content floor so clipped diagnostics stay usable.

const PaneScript := preload("res://game/debug/pages/debug_stats_page.gd")
const OverlayScript := preload("res://game/debug/nova_debug_overlay.gd")
const MissionPresentation := preload("res://game/world/mission_presentation.gd")


class DestructionInfoStub:
	extends RefCounted
	var husk_swaps := 2
	var debris_triangles := 4
	var glass_points := 1


class FireInfoStub:
	extends RefCounted
	var fires := 2
	var sounds := 1


class ThrowableInfoStub:
	extends RefCounted
	var live := 5


# Typed test doubles (ADR 0034): the page reads the GameWorld/MissionPresentation
# classes, so the doubles ARE those classes with the counter getters
# overridden. The effect world is a real (empty) EffectWorld — 0 live still
# renders a non-empty info cell.
class WorldInfoHarness:
	extends GameWorld
	var effect := EffectWorld.new()
	func get_runtime_perf_counters() -> Dictionary:
		return {"runtime": {"sim": {"present_entity_count": 7}}}
	func get_effect_world() -> EffectWorld:
		return effect
	func get_fire_present_stats() -> RefCounted:
		return FireInfoStub.new()
	func get_destruction_present_stats() -> RefCounted:
		return DestructionInfoStub.new()


class RuntimeInfoHarness:
	extends MissionPresentation
	func get_throwable_present_stats() -> RefCounted:
		return ThrowableInfoStub.new()
	func get_wire_present_stats() -> WirePresentStats:
		return WirePresentStats.create(4, 0, 1)


func _make_pane(ctx: DebugContext = DebugContext.new()) -> DebugStatsPage:
	var pane: DebugStatsPage = PaneScript.new()
	pane.setup(ctx)
	add_child_autofree(pane)
	return pane


func _blank_window() -> Array:
	var sums := PackedInt64Array()
	sums.resize(FrameStatsBoard.SLOT_COUNT)
	var peaks := PackedInt64Array()
	peaks.resize(FrameStatsBoard.SLOT_COUNT)
	var counts := PackedInt32Array()
	counts.resize(FrameStatsBoard.SLOT_COUNT)
	return [sums, peaks, counts]


func _row(pane: DebugStatsPage, id: StringName) -> DebugStatsDisplayRow:
	for row in pane.get_display_snapshot():
		if row.id == id:
			return row
	return null


func _find_tree_item(item: TreeItem, label: String) -> TreeItem:
	if item.get_text(0) == label:
		return item
	for child in item.get_children():
		var found := _find_tree_item(child, label)
		if found != null:
			return found
	return null


func test_rows_cover_major_systems_and_label_units() -> void:
	var pane := _make_pane()
	for id in [&"frame", &"world", &"foliage", &"runtime", &"sim",
			&"host_prep", &"host_pump", &"host_receive", &"host_connections",
			&"host_adapter", &"server_tick", &"server_input", &"server_world",
			&"world_setup", &"world_scripts", &"world_ai", &"ai_reactions",
			&"ai_collision", &"ai_entities", &"ai_infantry",
			&"ai_infantry_remote", &"ai_infantry_combat",
			&"ai_infantry_animation", &"ai_infantry_collision",
			&"ai_infantry_collision_contacts", &"ai_infantry_collision_repulsion",
			&"ai_infantry_collision_ground", &"ai_infantry_collision_unattributed",
			&"ai_infantry_unattributed", &"ai_other_entities",
			&"ai_entities_unattributed", &"ai_auth_vehicles", &"ai_vehicle_scan",
			&"ai_vehicle_motors", &"ai_vehicle_riders", &"ai_vehicles_unattributed",
			&"ai_client_vehicles", &"ai_events", &"ai_unattributed",
			&"world_attachments", &"attachment_orphans", &"attachment_children",
			&"attachment_riders", &"attachment_unattributed",
			&"world_throwables", &"world_weapons", &"world_projectiles",
			&"world_destruction", &"world_housekeeping", &"world_unattributed",
			&"match", &"server_rules", &"server_replication",
			&"replication_query_prep", &"replication_query_collect",
			&"replication_query_grid", &"replication_query_grid_span",
			&"replication_query_grid_bucket", &"replication_query_grid_workspace",
			&"replication_query_grid_unattributed", &"replication_query_unattributed",
			&"replication_snapshot",
			&"replication_fan", &"replication_fan_setup", &"replication_rounds",
			&"replication_entities", &"replication_entity_setup",
			&"replication_entity_score", &"replication_entity_los",
			&"replication_entity_los_terrain", &"replication_entity_los_sector",
			&"replication_entity_los_unattributed",
			&"replication_entity_score_math", &"replication_entity_sort",
			&"replication_entity_budget", &"replication_entity_unattributed",
			&"replication_encode", &"replication_enqueue",
			&"replication_fan_unattributed", &"replication_unattributed",
			&"server_unattributed", &"host_send", &"host_unattributed",
			&"host_player", &"net", &"client_setup", &"client_receive",
			&"client_maintenance", &"client_send", &"client_unattributed",
			&"adm_resolve", &"sim_unattributed", &"sim_sink",
			&"trace", &"trace_terrain", &"trace_static", &"trace_dynamic",
			&"trace_person", &"effects_drain", &"effects", &"present",
			&"snapshot", &"mission_rows", &"mission_rows_core",
			&"mission_rows_aim", &"mission_rows_controls",
			&"mission_rows_visibility", &"mission_rows_body",
			&"mission_rows_remainder",
			&"wire_rows", &"fire",
			&"destruction", &"throwable", &"runtime_overhead",
			&"local_view", &"framefx", &"scene_env", &"terrain", &"network_frame",
			&"occl", &"occl_build",
			&"occl_probe", &"occl_apply", &"occl_glue",
			&"occl_building_query", &"occl_building_apply",
			&"occl_cull_query", &"occl_cull_apply", &"occl_light_query",
			&"occl_light_apply", &"occl_water_apply", &"occl_apply_remainder",
			&"env", &"sun_veil", &"light", &"material",
			&"model_clock_animation", &"model_panm", &"model_material",
			&"model_order_bounds",
			&"model_runtime_remainder", &"slot_shadow",
			&"particles", &"audio", &"clear", &"env_cube", &"world_remainder",
			&"hud", &"stats_sample", &"shell_control", &"round_flow",
			&"menu_shell", &"menu_video", &"debug_refresh", &"frame_overhead",
			&"other_process", &"physics_callbacks", &"deferred_flush",
			&"hud_draw_compile", &"hud_draw_emit", &"render_draw",
			&"pacing_input", &"engine_frame",
			&"render", &"render_main", &"render_shadow",
			&"render_water", &"render_q3", &"render_q3_cpu", &"render_q3_gpu",
			&"render_slot", &"render_slot_cpu", &"render_slot_gpu",
			&"env_nodes", &"water"]:
		assert_not_null(_row(pane, id), "the Stats tab carries a '%s' row" % id)
	assert_eq(pane.stats_tree.get_column_title(0), "System")
	assert_eq(pane.stats_tree.get_column_title(1), "Avg")
	assert_eq(pane.stats_tree.get_column_title(2), "Peak")
	assert_eq(pane.stats_tree.get_column_title(3), "Info")
	assert_eq(pane.stats_tree.get_column_title_tooltip_text(1),
			"Average milliseconds per frame")
	assert_eq(pane.stats_tree.get_column_title_tooltip_text(2),
			"Peak milliseconds in one frame")
	assert_eq(_row(pane, &"trace").label, "Projectile trace (attributed)",
			"the group does not claim the intentionally uncharged setup/water time")
	assert_eq(_row(pane, &"other_process").label, "Other process callbacks")
	assert_eq(_row(pane, &"deferred_flush").label,
			"Deferred flush (draw callbacks, transforms)")
	assert_eq(_row(pane, &"render_draw").label,
			"RenderingServer draw (all viewports)")
	assert_eq(_row(pane, &"pacing_input").label, "Servers/input/pacing")
	assert_eq(_row(pane, &"engine_frame").label, "Unattributed engine time")


func test_model_and_mission_row_info_cells_render_their_counts() -> void:
	var pane: DebugStatsPage = PaneScript.new()
	pane.setup(DebugContext.new())
	add_child_autofree(pane)
	var window := _blank_window()
	var sums: PackedInt64Array = window[0]
	var counts: PackedInt32Array = window[2]
	counts[FrameStatsBoard.MODEL_AWAKE_MODELS] = 2
	sums[FrameStatsBoard.MODEL_AWAKE_MODELS] = 24
	counts[FrameStatsBoard.MODEL_RENDERABLE_MODELS] = 2
	sums[FrameStatsBoard.MODEL_RENDERABLE_MODELS] = 16
	counts[FrameStatsBoard.PRESENT_MISSION_ROWS] = 1
	sums[FrameStatsBoard.PRESENT_MISSION_ROWS] = 40
	sums[FrameStatsBoard.PRESENT_MISSION_SUBMITTED_ROWS] = 30
	sums[FrameStatsBoard.PRESENT_MISSION_BODY_ROWS] = 20
	pane.render_window(1, sums, window[1], counts, null, null)
	var material := _find_tree_item(pane.stats_tree.get_root(), "Model runtime")
	assert_not_null(material)
	if material != null:
		assert_eq(material.get_text(3), "12 awake · 8 renderable",
				"the model cell averages per sample with the U+00B7 separator")
	var rows := _find_tree_item(pane.stats_tree.get_root(), "Mission rows")
	assert_not_null(rows)
	if rows != null:
		assert_eq(rows.get_text(3),
				"40 rows · 30 submitted · 20 body",
				"the mission-row cell averages per frame with the U+00B7 separator")


func test_narrow_tree_stays_inside_the_page_and_tooltips_keep_full_text() -> void:
	var mount := Control.new()
	mount.size = Vector2(252, 480)
	add_child_autofree(mount)
	var pane: DebugStatsPage = PaneScript.new()
	pane.setup(DebugContext.new())
	mount.add_child(pane)
	pane.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	await get_tree().process_frame
	await get_tree().process_frame

	assert_lte(pane.get_combined_minimum_size().x, mount.size.x,
			"the Stats page fits the narrow debug content column")
	assert_lte(pane.stats_tree.position.x + pane.stats_tree.size.x, pane.size.x,
			"the Stats tree does not extend past the page")
	var used_width := 0
	for column in range(pane.stats_tree.columns):
		used_width += pane.stats_tree.get_column_width(column)
	assert_lte(used_width, int(pane.stats_tree.size.x),
			"the columns fit without a horizontal overflow strip")
	assert_false(pane.stats_tree.scroll_horizontal_enabled,
			"narrow Stats stays vertically scrollable without sideways navigation")
	assert_eq(pane.stats_tree.focus_mode, Control.FOCUS_ALL,
			"keyboard users can navigate and expand Stats rows")

	var window := _blank_window()
	var sums: PackedInt64Array = window[0]
	var counts: PackedInt32Array = window[2]
	counts[FrameStatsBoard.TRACE_CALLS] = 1
	sums[FrameStatsBoard.TRACE_CALLS] = 1
	pane.render_window(1, sums, window[1], counts, null, null)
	var trace := _find_tree_item(
			pane.stats_tree.get_root(), "Projectile trace (attributed)")
	assert_not_null(trace)
	if trace == null:
		return
	assert_eq(trace.get_tooltip_text(0), "Projectile trace (attributed)")
	assert_eq(trace.get_tooltip_text(1), trace.get_text(1))
	assert_eq(trace.get_tooltip_text(2), trace.get_text(2),
			"clipped numeric cells retain their complete values")
	assert_ne(trace.get_text(3), "")
	assert_eq(trace.get_tooltip_text(3), trace.get_text(3),
			"the full live-info value remains available when its cell is clipped")


func test_render_window_formats_average_peak_groups_and_residual() -> void:
	var pane := _make_pane()
	var window := _blank_window()
	var sums: PackedInt64Array = window[0]
	var peaks: PackedInt64Array = window[1]
	var counts: PackedInt32Array = window[2]
	sums[FrameStatsBoard.SIM_STEP] = 20_000
	peaks[FrameStatsBoard.SIM_STEP] = 5_000
	counts[FrameStatsBoard.SIM_STEP] = 10
	var sim_parts := {
		FrameStatsBoard.SIM_HOST_PREP: 1_000,
		FrameStatsBoard.SIM_HOST_PUMP: 12_000,
		FrameStatsBoard.SIM_HOST_RECEIVE: 1_000,
		FrameStatsBoard.SIM_HOST_CONNECTIONS: 1_000,
		FrameStatsBoard.SIM_HOST_ADAPTER: 1_000,
		FrameStatsBoard.SIM_SERVER_TICK: 7_000,
		FrameStatsBoard.SIM_SERVER_INPUT: 1_000,
		FrameStatsBoard.SIM_SERVER_WORLD: 3_000,
		FrameStatsBoard.SIM_WORLD_SETUP: 250,
		FrameStatsBoard.SIM_WORLD_SCRIPTS: 250,
		FrameStatsBoard.SIM_WORLD_AI: 1_500,
		FrameStatsBoard.SIM_WORLD_ATTACHMENTS: 100,
		FrameStatsBoard.SIM_WORLD_THROWABLES: 100,
		FrameStatsBoard.SIM_WORLD_WEAPONS: 100,
		FrameStatsBoard.SIM_WORLD_PROJECTILES: 100,
		FrameStatsBoard.SIM_WORLD_DESTRUCTION: 50,
		FrameStatsBoard.SIM_WORLD_HOUSEKEEPING: 50,
		FrameStatsBoard.SIM_MATCH: 500,
		FrameStatsBoard.SIM_SERVER_RULES: 1_000,
		FrameStatsBoard.SIM_SERVER_REPLICATION: 1_000,
		FrameStatsBoard.SIM_HOST_SEND: 1_000,
		FrameStatsBoard.SIM_HOST_PLAYER: 1_000,
		FrameStatsBoard.SIM_NET: 1_000,
		FrameStatsBoard.SIM_ADM_RESOLVE: 1_000,
	}
	for slot in sim_parts:
		sums[slot] = sim_parts[slot]
		counts[slot] = 10
	sums[FrameStatsBoard.OCCL_BUILD] = 2_000
	sums[FrameStatsBoard.OCCL_PROBE] = 3_000
	sums[FrameStatsBoard.OCCL_APPLY] = 4_000
	sums[FrameStatsBoard.OCCL_GLUE] = 1_000
	for slot in [FrameStatsBoard.OCCL_BUILD, FrameStatsBoard.OCCL_PROBE,
			FrameStatsBoard.OCCL_APPLY, FrameStatsBoard.OCCL_GLUE]:
		counts[slot] = 10
	sums[FrameStatsBoard.FRAME_WALL] = 100_000
	counts[FrameStatsBoard.FRAME_WALL] = 10
	sums[FrameStatsBoard.FRAME_WORLD] = 60_000
	counts[FrameStatsBoard.FRAME_WORLD] = 10
	sums[FrameStatsBoard.FRAME_HUD] = 10_000
	counts[FrameStatsBoard.FRAME_HUD] = 10
	sums[FrameStatsBoard.FRAME_SHELL_CONTROL] = 5_000
	counts[FrameStatsBoard.FRAME_SHELL_CONTROL] = 10
	sums[FrameStatsBoard.FRAME_PROCESS_CALLBACKS] = 85_000
	counts[FrameStatsBoard.FRAME_PROCESS_CALLBACKS] = 10
	sums[FrameStatsBoard.FRAME_PHYSICS_CALLBACKS] = 5_000
	counts[FrameStatsBoard.FRAME_PHYSICS_CALLBACKS] = 10
	# The engine time outside the callbacks, split at the draw signals: 0.3 +
	# 0.5 + 0.1 ms of the 1.0 ms residual are attributed, 0.1 ms is not.
	sums[FrameStatsBoard.FRAME_DEFERRED_FLUSH] = 3_000
	counts[FrameStatsBoard.FRAME_DEFERRED_FLUSH] = 10
	sums[FrameStatsBoard.FRAME_DRAW] = 5_000
	counts[FrameStatsBoard.FRAME_DRAW] = 10
	sums[FrameStatsBoard.FRAME_PACING_INPUT] = 1_000
	counts[FrameStatsBoard.FRAME_PACING_INPUT] = 10
	sums[FrameStatsBoard.FRAME_TIME_PROCESS] = 930_000
	counts[FrameStatsBoard.FRAME_TIME_PROCESS] = 10
	sums[FrameStatsBoard.FRAME_PHYSICS_SERVER] = 2_000
	counts[FrameStatsBoard.FRAME_PHYSICS_SERVER] = 10
	sums[FrameStatsBoard.FRAME_PHYSICS_ITERATIONS] = 15
	counts[FrameStatsBoard.FRAME_PHYSICS_ITERATIONS] = 10

	pane.render_window(10, sums, peaks, counts, null, null)
	assert_eq(_row(pane, &"sim").average, "2.00")
	assert_eq(_row(pane, &"sim").peak, "5.00")
	assert_eq(_row(pane, &"sim_unattributed").average, "0.40")
	assert_eq(_row(pane, &"host_unattributed").average, "0.10")
	assert_eq(_row(pane, &"server_unattributed").average, "0.05")
	assert_eq(_row(pane, &"world_unattributed").average, "0.05")
	assert_eq(_row(pane, &"hud_attach").average, "-")
	assert_eq(_row(pane, &"occl").average, "1.00",
			"the group includes build, probe, apply, and binding glue")
	assert_eq(_row(pane, &"occl_glue").average, "0.10")
	assert_eq(_row(pane, &"other_process").average, "1.00")
	assert_eq(_row(pane, &"physics_callbacks").average, "0.50")
	assert_eq(_row(pane, &"physics_callbacks").info,
			"server max 0.20 ms · 1.5 iter/f")
	assert_eq(_row(pane, &"deferred_flush").average, "0.30")
	assert_eq(_row(pane, &"render_draw").average, "0.50")
	assert_eq(_row(pane, &"pacing_input").average, "0.10")
	assert_eq(_row(pane, &"engine_frame").average, "0.10",
			"the residual is what the three draw-signal spans did not bracket")
	assert_true(_row(pane, &"frame").info.ends_with("· process 93.00 ms"),
			"the frame row carries Godot's TIME_PROCESS cross-check")


func test_pass_count_cells_average_per_frame_and_clear_when_unsampled() -> void:
	var pane := _make_pane()
	var window := _blank_window()
	var sums: PackedInt64Array = window[0]
	var counts: PackedInt32Array = window[2]
	sums[FrameStatsBoard.RENDER_MAIN_OBJECTS] = 4_550
	sums[FrameStatsBoard.RENDER_MAIN_DRAWS] = 4_510
	counts[FrameStatsBoard.RENDER_MAIN_OBJECTS] = 10
	sums[FrameStatsBoard.RENDER_SHADOW_OBJECTS] = 0
	sums[FrameStatsBoard.RENDER_SHADOW_DRAWS] = 0
	counts[FrameStatsBoard.RENDER_SHADOW_OBJECTS] = 10
	sums[FrameStatsBoard.RENDER_WATER_OBJECTS] = 4_220
	sums[FrameStatsBoard.RENDER_WATER_DRAWS] = 4_180
	counts[FrameStatsBoard.RENDER_WATER_OBJECTS] = 10

	pane.render_window(10, sums, window[1], counts, null, null)
	assert_eq(_row(pane, &"render_main").info, "455 objs · 451 draws")
	assert_eq(_row(pane, &"render_shadow").info, "0 objs · 0 draws",
			"a sampled pass that rendered nothing reads zero, not blank")
	assert_eq(_row(pane, &"render_water").info, "422 objs · 418 draws")

	var blank := _blank_window()
	pane.render_window(10, blank[0], blank[1], blank[2], null, null)
	assert_eq(_row(pane, &"render_water").info, "",
			"an unsampled pass (no water in this world) clears its cell")
	assert_eq(_row(pane, &"render_main").info, "",
			"pass counts never survive a window with no samples")


func test_capture_follows_visibility_and_board_replacement() -> void:
	var pane := _make_pane()
	var first := FrameStatsBoard.new()
	var second := FrameStatsBoard.new()
	pane.set_frame_stats_board(first)
	assert_false(first.is_capture_active())
	pane.set_capture_active(true)
	assert_true(first.is_capture_active())
	pane.visible = false
	assert_false(first.is_capture_active())
	pane.visible = true
	assert_true(first.is_capture_active())

	pane.set_frame_stats_board(second)
	assert_false(first.is_capture_active(),
			"replacing a live board closes the old capture")
	assert_true(second.is_capture_active(),
			"the replacement inherits the pane's live capture state")
	pane.set_frame_stats_board(null)
	assert_false(second.is_capture_active(), "null detaches and closes the board")
	assert_false(pane.is_capturing())


func test_exit_tree_closes_capture() -> void:
	var pane: DebugStatsPage = PaneScript.new()
	pane.setup(DebugContext.new())
	add_child(pane)
	var board := FrameStatsBoard.new()
	pane.set_frame_stats_board(board)
	pane.set_capture_active(true)
	assert_true(board.is_capture_active())
	remove_child(pane)
	assert_false(board.is_capture_active(),
			"leaving the tree releases every capture-owned diagnostic")
	pane.free()


func test_overlay_selects_stats_without_exposing_pages() -> void:
	var overlay = add_child_autofree(OverlayScript.new(
			"user://test_stats_overlay_%d.cfg" % Time.get_ticks_usec()))
	var board := FrameStatsBoard.new()
	overlay.set_frame_stats_board(board)
	overlay.toggle()
	await get_tree().process_frame
	assert_false(board.is_capture_active(),
			"opening on another page leaves capture off")
	assert_true(overlay.select_page(&"Stats"))
	await get_tree().process_frame
	assert_true(overlay.is_stats_capturing())
	assert_true(board.is_capture_active())
	assert_false(overlay.select_page(&"DoesNotExist"))
	assert_gt(overlay.get_stats_display_snapshot().size(), 0)
	overlay.toggle()
	assert_false(board.is_capture_active())


func test_refresh_drains_the_board_window() -> void:
	var pane := _make_pane()
	var board := FrameStatsBoard.new()
	pane.set_frame_stats_board(board)
	pane.set_capture_active(true)
	board.add(FrameStatsBoard.SIM_STEP, 4_000)
	await get_tree().process_frame
	board.add(FrameStatsBoard.SIM_STEP, 4_000)
	await get_tree().process_frame
	pane.refresh()
	pane.refresh()
	assert_ne(_row(pane, &"sim").average, "-")
	assert_eq(board.drain().sums[FrameStatsBoard.SIM_STEP], 0,
			"the pane atomically drained the prior window")


func test_mission_scoped_info_clears_when_sources_disappear() -> void:
	var ctx := DebugContext.new()
	var pane := _make_pane(ctx)
	# Off-tree: the GameWorld script class alone has no scene children, and
	# the counter overrides need no tree presence.
	var world := WorldInfoHarness.new()
	autofree(world)
	autofree(world.effect)
	var runtime := RuntimeInfoHarness.new()
	add_child_autofree(runtime)
	ctx.world_source = func(): return world
	var window := _blank_window()
	var sums: PackedInt64Array = window[0]
	var counts: PackedInt32Array = window[2]
	sums[FrameStatsBoard.EFFECTS_DRAIN] = 2_000
	counts[FrameStatsBoard.EFFECTS_DRAIN] = 1
	# occl is sim-owned and needs a live occlusion mission, so this fixture
	# exercises the world/runtime-fed rows; occl still participates in the
	# clear pass below.
	pane.render_window(1, sums, window[1], counts, runtime, null)
	for id in [&"effects", &"fire", &"destruction", &"throwable",
			&"wire_rows"]:
		assert_ne(_row(pane, id).info, "", "%s received live mission info" % id)

	ctx.world_source = Callable()
	pane.render_window(1, sums, window[1], counts, null, null)
	for id in [&"effects", &"fire", &"destruction", &"throwable",
			&"wire_rows", &"occl"]:
		assert_eq(_row(pane, id).info, "",
				"%s does not retain the previous mission" % id)
