extends GutTest

# DebugPerfPane: the overlay's Perf tab over the PerfTimeline ring + live
# monitors. Fabricated timelines pin the span-tree shape and the
# rebuild-only-on-change contract. The static ring is shared and append-only
# across the whole GUT run, so tests use UNIQUE labels and never assert ring
# totals or ordering beyond their own entries.

const PaneScript := preload("res://engine/debug/debug_perf_pane.gd")
const OverlayScript := preload("res://engine/debug/nova_debug_overlay.gd")


func _make_pane() -> DebugPerfPane:
	var pane: DebugPerfPane = PaneScript.new()
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


func test_overlay_grows_a_perf_tab() -> void:
	var overlay = add_child_autofree(OverlayScript.new())
	var perf = overlay._tabs.get_node_or_null("Perf")
	assert_not_null(perf, "the overlay's fourth tab is the perf pane")
	assert_true(perf is DebugPerfPane)