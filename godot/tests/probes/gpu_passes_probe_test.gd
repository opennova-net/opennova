extends GutTest

# The gpu_passes probe's reduction (godot/probes/perf): each interval between two captured
# timestamps is charged to the label that opens it and to the viewport whose vp_begin_ /
# vp_end_ pair encloses it; means are per sampled frame, a viewport's total per frame drawn.

const Probe := preload("res://probes/perf/gpu_passes_probe.gd")


func _frame(offset_ns: int, with_target: bool) -> Array:
	var f: Array = [
		["> Render Viewports", offset_ns + 0],
		["vp_begin_7", offset_ns + 100_000],
		["Render Opaque Pass", offset_ns + 200_000],
		["Tonemap", offset_ns + 2_200_000],
		["vp_end_7", offset_ns + 2_700_000],
	]
	if with_target:
		f.append_array([
			["vp_begin_9", offset_ns + 2_800_000],
			["Render CanvasItems", offset_ns + 2_900_000],
			["vp_end_9", offset_ns + 3_400_000],
		])
	f.append(["< Render Viewports", offset_ns + 4_000_000])
	return f


func _passes(row) -> Dictionary:
	var out := {}
	for pair in row.passes:
		out[String(pair[0])] = float(pair[1])
	return out


func test_intervals_split_per_viewport_and_label() -> void:
	var frames: Array = [_frame(0, true), _frame(50_000_000, false)]
	var table = Probe.aggregate(frames, {"7": "/root/GameViewport"})
	assert_eq(table.frames, 2)
	assert_almost_eq(table.frame_ms, 4.0, 1e-9)
	var surface = table.owners["/root/GameViewport"]
	assert_almost_eq(surface.total_ms, 2.6, 1e-9, "vp_end - vp_begin, per frame drawn")
	assert_eq(surface.frames, 2)
	var passes := _passes(surface)
	assert_almost_eq(passes["Render Opaque Pass"], 2.0, 1e-9)
	assert_almost_eq(passes["Tonemap"], 0.5, 1e-9)
	assert_almost_eq(passes["vp_begin_7"], 0.1, 1e-9)
	assert_eq(String(surface.passes[0][0]), "Render Opaque Pass", "largest first")
	# An unnamed viewport keeps its RID id; drawn in one frame of two.
	var target = table.owners["viewport 9"]
	assert_eq(target.frames, 1)
	assert_almost_eq(target.total_ms, 0.6, 1e-9)
	# The frame's own intervals outside every pair: the second frame's 2.7..4.0 ms gap
	# and the first's 3.4..4.0 and 2.7..2.8 ms gaps, over two frames.
	var frame_passes := _passes(table.owners[Probe.FRAME_OWNER])
	assert_almost_eq(frame_passes["> Render Viewports"], 0.1, 1e-9)
	assert_almost_eq(frame_passes["vp_end_7"], (0.1 + 1.3) / 2.0, 1e-9)
	assert_almost_eq(frame_passes["vp_end_9"], 0.3, 1e-9)
	# Largest view first; the verdict's data carries every row.
	assert_eq(table.rows_by_total()[0].name, "/root/GameViewport")
	var data: Dictionary = table.to_data()
	assert_eq(int(data["frames"]), 2)
	assert_eq((data["owners"] as Dictionary).size(), 3)


func test_a_frame_with_one_timestamp_adds_nothing() -> void:
	var table = Probe.aggregate([[["only", 5]]], {})
	assert_eq(table.frames, 1)
	assert_almost_eq(table.frame_ms, 0.0, 1e-9)
	assert_true(table.owners.is_empty())
