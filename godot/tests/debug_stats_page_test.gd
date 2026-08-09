extends GutTest

# DebugStatsPage: semantic rows and capture lifecycle are exercised through the
# public value interface. The focused layout case observes the rendered Tree at
# the overlay's narrow content floor so clipped diagnostics stay usable.

const PaneScript := preload("res://game/debug/pages/debug_stats_page.gd")
const OverlayScript := preload("res://game/debug/nova_debug_overlay.gd")
const MissionRuntime := preload("res://game/world/mission_runtime.gd")


class DestructionInfoStub:
	extends RefCounted
	var husk_swaps := 2
	var bursts := 4


class ThrowableInfoStub:
	extends RefCounted
	var live := 5


# Typed test doubles (ADR 0034): the page reads the GameWorld/MissionRuntime
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
	func get_fire_present_stats() -> Dictionary:
		return {"fires": 2, "sounds": 1}
	func get_destruction_present_stats() -> RefCounted:
		return DestructionInfoStub.new()


class RuntimeInfoHarness:
	extends MissionRuntime
	func get_throwable_present_stats() -> RefCounted:
		return ThrowableInfoStub.new()
	func get_wire_present_stats() -> WirePresentStats:
		return WirePresentStats.new(4, 0, 1)


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
	for id in [&"frame", &"world", &"foliage", &"runtime", &"sim", &"net",
			&"trace", &"trace_terrain", &"trace_static", &"trace_dynamic",
			&"trace_person", &"effects_drain", &"effects", &"present",
			&"snapshot", &"mission_rows", &"wire_rows", &"fire",
			&"destruction", &"throwable", &"occl", &"occl_build",
			&"occl_probe", &"occl_apply", &"occl_glue", &"env", &"audio",
			&"hud", &"render", &"render_main", &"render_shadow",
			&"render_water"]:
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

	pane.render_window(10, sums, peaks, counts, null, null)
	assert_eq(_row(pane, &"sim").average, "2.00")
	assert_eq(_row(pane, &"sim").peak, "5.00")
	assert_eq(_row(pane, &"hud_attach").average, "-")
	assert_eq(_row(pane, &"occl").average, "1.00",
			"the group includes build, probe, apply, and binding glue")
	assert_eq(_row(pane, &"occl_glue").average, "0.10")
	assert_eq(_row(pane, &"shell_residual").average, "3.00")


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
