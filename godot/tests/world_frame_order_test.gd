extends GutTest

# The ONE static frame-leg table (ADR 0043 d9): GameWorld runs its device legs
# in this witnessed order around inmatch::Session::advance() (the "session"
# row), and the same loop replays the camera-only rows for a frozen-pose
# capture. The table is data on the class; these pins hold its order, its stop
# rows, and the local-view-first rule (D-RORD-8).


func test_frame_leg_table_is_the_witnessed_order() -> void:
	assert_eq(Array(GameWorld.frame_leg_names()), [
		"begin", "session", "local_view", "scene_environment",
		"environment_nodes", "terrain", "water", "network", "blink",
		"occlusion", "foliage", "iris", "sun_veil", "lights", "materials", "framefx",
		"slot_shadows", "particles", "precipitation", "scene_overlay",
		"screen_effects", "audio", "clear", "environment_cube", "finish",
	])


func test_local_view_precedes_every_camera_consumer() -> void:
	var names := Array(GameWorld.frame_leg_names())
	var local_view := names.find("local_view")
	assert_gt(local_view, names.find("session"),
			"the local view is placed from the state THIS frame's session tick produced")
	for consumer in ["terrain", "foliage", "occlusion", "iris", "framefx", "particles"]:
		assert_gt(names.find(consumer), local_view,
				"%s samples the post-present camera" % consumer)
	assert_lt(names.find("terrain"), names.find("water"),
			"terrain tracks the visible bounds the water leg's g_WaterActive test reads")
	assert_lt(names.find("water"), names.find("foliage"),
			"foliage consumes this frame's detail-cell handoff after terrain")
	assert_gt(names.find("screen_effects"), names.find("particles"),
			"the FrameFX plan reads this frame's published distortion content")
	assert_lt(names.find("occlusion"), names.find("foliage"),
			"foliage anchors its MODEL tier on the entities this frame's occlusion admitted")
	for producer in ["lights", "particles", "precipitation"]:
		assert_gt(names.find("scene_overlay"), names.find(producer),
				"the overlay tail gathers what %s published this frame" % producer)


func test_only_the_session_and_network_rows_stop_the_frame() -> void:
	for name in GameWorld.frame_leg_names():
		var stops := GameWorld.frame_leg_stops_frame(name)
		if name == "session" or name == "network":
			assert_true(stops, "%s stops the frame on a terminal outcome" % name)
		else:
			assert_false(stops, "%s never stops the frame" % name)
	assert_false(GameWorld.frame_leg_stops_frame("no_such_leg"))


func test_frozen_pose_replay_keeps_the_camera_producer_order() -> void:
	var replay := Array(GameWorld.frozen_pose_leg_names())
	assert_eq(replay, [
		"celestial_settle", "sun_veil", "iris_stamp", "weather_settle",
		"scene_environment", "terrain", "occlusion", "foliage", "sky_settle",
		"water_settle", "particles", "lights", "slot_shadows", "scene_overlay",
		"clear",
	])
	var live := Array(GameWorld.frame_leg_names())
	for time_owner in ["session", "environment_nodes", "materials", "audio", "network", "blink"]:
		assert_true(live.has(time_owner))
		assert_false(replay.has(time_owner),
				"the replay omits the time-owning %s leg" % time_owner)
