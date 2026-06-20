extends GutTest

# NovaNetClient (engine/network/nova_net_client.{h,cpp}) GDScript binding shape.
# The wire decode itself is covered by tests/novaworld/nw_replay_timeline_test
# (ctest); this asserts the GDExtension surface the Godot spectator renders from,
# including the event + environment streams added so the in-engine setup replaces
# the old standalone HTML replay viewer (now removed).


func _client():
	var c = NovaNetClient.new()
	autofree(c)
	return c


func test_fresh_client_has_empty_world() -> void:
	var c = _client()
	assert_eq(c.get_entity_count(), 0, "no entities before any wire data")
	assert_eq(c.get_entities().size(), 0)
	assert_eq(c.get_state(), NovaNetClient.STATE_IDLE)
	assert_eq(c.get_mission(), "", "mission name empty until read off the wire")


func test_get_events_empty_on_fresh_client() -> void:
	var c = _client()
	var events: Array = c.get_events()
	assert_eq(events.size(), 0, "no events before any wire data")


func test_env_at_not_found_on_fresh_client() -> void:
	var c = _client()
	var env: Dictionary = c.env_at(0.0)
	assert_false(bool(env.get("found", true)),
		"env unavailable until the wire carries an env block")


func test_sample_at_missing_handle_not_found() -> void:
	var c = _client()
	var s: Dictionary = c.sample_at(0x0005, 0.0)
	assert_false(bool(s.get("found", true)),
		"sampling an unknown handle reports not found")


func test_frame_range_is_vector2i() -> void:
	var c = _client()
	assert_eq(typeof(c.get_frame_range()), TYPE_VECTOR2I)
	assert_eq(c.get_latest_frame(), 0, "render head at 0 before any frame folds in")
