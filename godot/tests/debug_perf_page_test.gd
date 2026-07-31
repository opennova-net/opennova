extends GutTest

# DebugPerfPage: the overlay's Perf page over the PerfTimeline ring + live
# monitors. Fabricated timelines pin the span-tree shape and the
# rebuild-only-on-change contract. The static ring is shared and append-only
# across the whole GUT run, so tests use UNIQUE labels and never assert ring
# totals or ordering beyond their own entries.

const PaneScript := preload("res://engine/debug/pages/debug_perf_page.gd")
const OverlayScript := preload("res://engine/debug/nova_debug_overlay.gd")


func _make_pane() -> DebugPerfPage:
	var pane: DebugPerfPage = PaneScript.new()
	pane.setup(NovaDebugContext.new())
	add_child_autofree(pane)
	return pane


func _fabricate(label: String) -> PerfTimeline:
	var timeline := PerfTimeline.begin(label)
	timeline.span("terrain")
	timeline.span("textures")  # nested under terrain (depth 1)
	timeline.end_span()
	timeline.end_span()
	timeline.span("objects")
	timeline.end_span()
	timeline.finish()
	return timeline


func _tree_texts(tree: Tree) -> Array:
	var out: Array = []
	var root := tree.get_root()
	if root == null:
		return out
	_collect(root, out)
	return out


func _collect(item: TreeItem, out: Array) -> void:
	var child := item.get_first_child()
	while child != null:
		out.append(child.get_text(0))
		_collect(child, out)
		child = child.get_next()


func test_render_timeline_builds_the_span_tree_by_depth() -> void:
	var pane := _make_pane()
	var timeline := _fabricate("perf pane shape test")
	pane.render_timeline(timeline)
	var texts := _tree_texts(pane.span_tree)
	assert_has(texts, "perf pane shape test", "the total row carries the load's label")
	assert_has(texts, "terrain")
	assert_has(texts, "textures")
	assert_has(texts, "objects")

	# Depth nesting: "textures" is a child of "terrain", not a sibling.
	var root := pane.span_tree.get_root()
	var total := root.get_first_child()
	var terrain := total.get_first_child()
	assert_eq(terrain.get_text(0), "terrain")
	assert_eq(terrain.get_first_child().get_text(0), "textures",
		"depth-1 spans nest under their depth-0 stage")
	assert_string_contains(terrain.get_text(1), "ms", "stages show milliseconds")


func test_span_tree_keeps_time_readable_in_a_narrow_perf_page() -> void:
	var mount := Control.new()
	mount.size = Vector2(388, 560)
	add_child_autofree(mount)
	var pane: DebugPerfPage = PaneScript.new()
	pane.setup(NovaDebugContext.new())
	mount.add_child(pane)
	pane.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	var long_label := \
			"Mission load with a deliberately long authored path and diagnostic context ".repeat(4)
	pane.render_timeline(_fabricate(long_label))
	await get_tree().process_frame
	await get_tree().process_frame

	assert_lte(pane.get_combined_minimum_size().x, mount.size.x,
			"live span text cannot widen the Perf page past its dock")
	assert_lte(pane.span_tree.position.x + pane.span_tree.size.x, pane.size.x,
			"the span tree remains inside the narrow content column")
	var used_width := pane.span_tree.get_column_width(0) \
			+ pane.span_tree.get_column_width(1)
	assert_lte(used_width, int(pane.span_tree.size.x),
			"Stage cannot push Time beyond the visible tree")
	assert_false(pane.span_tree.scroll_horizontal_enabled,
			"span diagnostics stay readable without sideways navigation")
	for column in range(pane.span_tree.columns):
		assert_true(pane.span_tree.is_column_clipping_content(column),
				"column %d clips display text instead of growing the tree" % column)
	assert_gte(pane.span_tree.get_column_width(1), 88,
			"Time keeps enough visible width for formatted durations")
	assert_eq(pane.span_tree.get_column_title_alignment(1),
			HORIZONTAL_ALIGNMENT_RIGHT)

	var total := pane.span_tree.get_root().get_first_child()
	assert_eq(total.get_text_alignment(1), HORIZONTAL_ALIGNMENT_RIGHT)
	assert_eq(total.get_tooltip_text(0), long_label,
			"the complete clipped stage remains available on hover")
	assert_eq(total.get_tooltip_text(1), total.get_text(1),
			"the complete duration remains available on hover")


func test_unbalanced_timeline_renders_cleanly() -> void:
	# finish() auto-closes spans an early return left open; the tree must not
	# choke on them.
	var timeline := PerfTimeline.begin("perf pane unbalanced test")
	timeline.span("parse")
	timeline.span("orphan")
	timeline.finish()
	var pane := _make_pane()
	pane.render_timeline(timeline)
	var texts := _tree_texts(pane.span_tree)
	assert_has(texts, "parse")
	assert_has(texts, "orphan")


func test_monitors_populate() -> void:
	var pane := _make_pane()
	pane.refresh_monitors()
	for key in ["fps", "frame_ms", "objects", "nodes", "video_mem"]:
		var label: Label = pane.monitor_labels[key]
		assert_false(label.text.is_empty(), "monitor '%s' shows a value" % key)
	assert_string_contains((pane.monitor_labels["frame_ms"] as Label).text, "ms")


func test_monitor_values_stay_visible_beside_clipped_names_at_narrow_width() -> void:
	var mount := Control.new()
	mount.size = Vector2(388, 900)
	add_child_autofree(mount)
	var pane: DebugPerfPage = PaneScript.new()
	pane.setup(NovaDebugContext.new())
	mount.add_child(pane)
	pane.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	var long_label := \
			"Mission load with objects effects terrain textures and a long authored path ".repeat(5)
	pane.render_history([_fabricate(long_label)])
	pane.refresh_monitors()
	await get_tree().process_frame
	await get_tree().process_frame

	var mount_rect := mount.get_global_rect()
	assert_lte(pane.get_combined_minimum_size().x, mount.size.x,
			"load summaries and monitor text cannot widen Perf past the dock")
	assert_lte(pane.history_option.get_global_rect().end.x, mount_rect.end.x,
			"the retained-load selector clips instead of widening every row")
	assert_string_contains(pane.history_option.tooltip_text, long_label,
			"the complete clipped load summary remains available on hover")

	for key in pane.monitor_labels:
		var value := pane.monitor_labels[key] as Label
		var row := value.get_parent() as HBoxContainer
		var name_label := row.get_node_or_null("Name") as Label
		assert_not_null(name_label, "%s has a stable readable name cell" % key)
		if name_label == null:
			continue
		assert_lte(row.get_global_rect().end.x, mount_rect.end.x,
				"%s row remains inside the content column" % key)
		assert_lte(value.get_global_rect().end.x, mount_rect.end.x,
				"%s value is not pushed beyond the dock" % key)
		assert_gte(value.size.x, 88.0,
				"%s reserves a readable value lane" % key)
		assert_eq(value.horizontal_alignment, HORIZONTAL_ALIGNMENT_RIGHT)
		assert_eq(value.tooltip_text, value.text,
				"%s retains the complete live value on hover" % key)
		assert_true(name_label.clip_text,
				"%s name clips before stealing value space" % key)
		assert_eq(name_label.tooltip_text, name_label.text,
				"%s retains the complete name on hover" % key)


func test_history_selector_lists_entries_and_renders_selection() -> void:
	var pane := _make_pane()
	var older := _fabricate("perf pane history older")
	var newer := _fabricate("perf pane history newer")
	pane.render_history([newer, older])  # most-recent-first, the ring's shape
	assert_eq(pane.history_option.item_count, 2)
	assert_string_contains(pane.history_option.get_item_text(0), "newer",
		"the newest load lists first and renders by default")
	assert_has(_tree_texts(pane.span_tree), "perf pane history newer")

	pane.history_option.select(1)
	pane.history_option.item_selected.emit(1)
	assert_has(_tree_texts(pane.span_tree), "perf pane history older",
		"picking an older entry re-renders its tree")


func test_steady_state_refresh_never_rebuilds_the_tree() -> void:
	var pane := _make_pane()
	var timeline := _fabricate("perf pane steady state")
	pane.render_history([timeline])
	var terrain_item := pane.span_tree.get_root().get_first_child().get_first_child()
	pane.render_history([timeline])
	pane.render_history([timeline])
	assert_eq(pane.span_tree.get_root().get_first_child().get_first_child(), terrain_item,
		"an unchanged ring keeps the same tree items (no per-refresh rebuild)")


func test_refresh_is_the_overlays_entry_point() -> void:
	# refresh() is what the overlay actually calls each cycle: ring + monitors.
	var pane := _make_pane()
	var _timeline := _fabricate("perf pane refresh entry")
	pane.refresh()
	assert_gt(pane.history_option.item_count, 0, "refresh pulls the static ring")
	assert_has(_tree_texts(pane.span_tree), "perf pane refresh entry",
		"...rendering the newest load by default")
	assert_false((pane.monitor_labels["fps"] as Label).text.is_empty(),
		"...and the live monitors")


func test_overlay_refreshes_perf_without_a_live_sim() -> void:
	# The ring is mount-wide state: "that load was slow, let me look" must work
	# from the menu, after the mission (and its runtime) are gone.
	var overlay = add_child_autofree(OverlayScript.new(
			"user://test_perf_overlay_%d.cfg" % Time.get_ticks_usec()))
	var _timeline := _fabricate("perf pane menu state")
	overlay.toggle()
	assert_true(overlay._status_label.visible, "no sim - the overlay says so")
	var page_list := overlay.find_child("PageList", true, false) as ItemList
	var compact_picker := overlay.find_child(
			"CompactPagePicker", true, false) as OptionButton
	assert_true(page_list.visible or compact_picker.visible,
			"...but responsive page navigation stays usable")
	assert_true(overlay.select_page(&"Perf"),
		"the Perf page selects with no runtime at all")
	assert_has(_tree_texts(overlay._perf_pane.span_tree), "perf pane menu state",
		"the perf page renders the retained load with no runtime at all")


func test_overlay_grows_a_perf_page() -> void:
	var overlay = add_child_autofree(OverlayScript.new(
			"user://test_perf_overlay_%d.cfg" % Time.get_ticks_usec()))
	var perf = overlay.find_child("Perf", true, false)
	assert_not_null(perf, "the overlay carries the perf page")
	assert_true(perf is DebugPerfPage)
