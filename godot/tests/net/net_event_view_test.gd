extends GutTest

# NetEventView (engine/world/net_event_view.gd): the 3D renderer of a
# NovaNetClient's decoded event stream that replaces the standalone HTML viewer's
# 2D markers. Driven off a stub client so the test is headless + asset-free; it
# asserts that live events draw geometry and that events past their fade window
# leave the marker mesh empty.

const NetEventView := preload("res://engine/world/net_event_view.gd")


class StubClient:
	var events: Array = []
	var frame: int = 0
	func get_events() -> Array: return events
	func get_latest_frame() -> int: return frame
	func sample_at(_handle, _frame) -> Dictionary: return {"found": false}


func _kill_event(at_frame: int) -> Dictionary:
	return {
		"frame": at_frame, "kind": NetEventView.KIND_KILL,
		"has_pos": true, "pos": Vector3(10.0, 20.0, 0.0), "has_dir": false,
		"source": 5, "target": 4, "aux": 65535,
		"event_type": 4, "sound": true, "label": "STRCND04",
	}


func _fire_event(at_frame: int) -> Dictionary:
	return {
		"frame": at_frame, "kind": NetEventView.KIND_FIRE,
		"has_pos": true, "pos": Vector3.ZERO, "has_dir": true, "dir": Vector2(1.0, 0.0),
		"source": 5, "target": 65535, "aux": 65535,
		"event_type": 0, "sound": true, "label": "",
	}


func _view(stub) -> Node3D:
	var v := NetEventView.new()
	v.setup(stub)
	add_child_autofree(v)
	return v


func _surfaces(v: Node3D) -> int:
	var mi := v.get_node("NetEventMarkers") as MeshInstance3D
	return (mi.mesh as ImmediateMesh).get_surface_count()


func test_no_events_draws_nothing() -> void:
	var v := _view(StubClient.new())
	v._process(0.0)
	assert_eq(_surfaces(v), 0, "an empty event list leaves the marker mesh empty")


func test_fresh_kill_draws_marker() -> void:
	var stub := StubClient.new()
	stub.frame = 100
	stub.events = [_kill_event(100)]
	var v := _view(stub)
	v._process(0.0)
	assert_eq(_surfaces(v), 1, "a fresh kill draws an X marker")


func test_kill_fades_out_past_window() -> void:
	var stub := StubClient.new()
	stub.events = [_kill_event(100)]
	stub.frame = 100 + NetEventView.KILL_FADE + 5
	var v := _view(stub)
	v._process(0.0)
	assert_eq(_surfaces(v), 0, "a kill older than KILL_FADE is gone")


func test_fire_tracer_draws_then_fades() -> void:
	var stub := StubClient.new()
	stub.frame = 50
	stub.events = [_fire_event(50)]
	var v := _view(stub)
	v._process(0.0)
	assert_eq(_surfaces(v), 1, "a fresh fire draws a tracer")
	stub.frame = 50 + NetEventView.FADE + 1
	v._process(0.0)
	assert_eq(_surfaces(v), 0, "the tracer is gone past FADE")
