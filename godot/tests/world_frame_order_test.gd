extends GutTest

# The ONE static frame-leg table (ADR 0043 d9): GameWorld runs its device legs
# in this witnessed order around inmatch::Session::advance() (the "session"
# row), and the same loop replays the camera-only rows for a frozen-pose
# capture. The table is data on the class; these pins hold its order, its stop
# rows, and the local-view-first rule (D-RORD-8).


func test_frame_leg_table_is_the_witnessed_order() -> void:
	assert_eq(Array(GameWorld.frame_leg_names()), [
		"begin", "session", "remote_admin", "scene_gate", "script_voice", "local_view",
		"facial", "scene_environment",
		"environment_nodes", "terrain", "water", "network", "blink",
		"occlusion", "foliage", "iris", "sun_veil", "lights", "ragdolls", "materials", "framefx",
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
	assert_eq(names.find("remote_admin"), names.find("session") + 1,
			"the remote admin's pump ends the main frame, right after the session's tick")
	assert_gt(names.find("ragdolls"), local_view,
			"the ragdolls read the avatar the local view posed this frame")
	assert_lt(names.find("ragdolls"), names.find("materials"),
			"the ragdolls pose their bodies before the materials leg writes the bones")
	# Render_ProcessMainSceneFrame calls Audio_UpdateAmbientStream (@0x5CA348) right ahead of
	# its camera compose (@0x5CA34D): the scene's first row.
	assert_eq(names.find("script_voice"), names.find("scene_gate") + 1,
			"the scene frame's voice update heads the scene rows")
	assert_eq(local_view, names.find("script_voice") + 1,
			"the camera compose follows the voice update")
	assert_true(GameWorld.frame_leg_draws_scene("script_voice"),
			"the voice update is the scene frame's, not the update's")
	# FaceAnim_RenderAllSlots runs in the scene frame after its camera compose (@0x5CA50C) and
	# again inside the Inset pass after that pass's own compose (@0x5C99BC): every drawn frame,
	# over the views the local view composed.
	assert_eq(names.find("facial"), local_view + 1,
			"the facial passes follow the compose that places both views")
	assert_true(GameWorld.frame_leg_draws_scene("facial"),
			"the facial passes are the scene frame's, not the update's")


# The status page stands in for retail's whole scene frame (GameLoop_RenderFrame draws
# Server_DrawStatusScreen in place of Render_ProcessMainSceneFrame; engine hud_server_status.h
# main_frame_draws_scene): the scene's rows are flagged, and the gate that reads the view word
# after the drain sits between the update's rows and the first of them.
func test_the_scene_rows_are_the_scene_frame() -> void:
	var names := Array(GameWorld.frame_leg_names())
	var kept := ["begin", "session", "remote_admin", "scene_gate", "network", "audio", "finish"]
	for name in names:
		assert_eq(GameWorld.frame_leg_draws_scene(name), not kept.has(name),
				"%s draws the scene: %s" % [name, not kept.has(name)])
	var gate := names.find("scene_gate")
	assert_eq(gate, names.find("remote_admin") + 1, "the gate reads the word after the drain")
	for name in names:
		if GameWorld.frame_leg_draws_scene(name):
			assert_gt(names.find(name), gate, "%s runs behind the gate" % name)
	assert_false(GameWorld.frame_leg_draws_scene("no_such_leg"))


# The scene frame's world block (Render_ProcessMainSceneFrame @0x5CA620..0x5CA640 jumps past
# it when the head's world-skip word is up: a cine plays, or the command map is over the
# menu): its state legs are flagged, all of them scene rows.
func test_the_world_block_rows() -> void:
	var world_rows := ["terrain", "water", "occlusion", "foliage", "sun_veil"]
	for name in Array(GameWorld.frame_leg_names()):
		assert_eq(GameWorld.frame_leg_in_world(name), world_rows.has(name),
				"%s is the world block's: %s" % [name, world_rows.has(name)])
		if GameWorld.frame_leg_in_world(name):
			assert_true(GameWorld.frame_leg_draws_scene(name), "%s is a scene row" % name)
	assert_false(GameWorld.frame_leg_in_world("no_such_leg"))


# A joiner's mission start ends in its final wait for the S2C 0x0F, a busy loop that renders
# nothing (Game_StartMission's wait @0x526295..0x5262E5; engine inmatch::Session::renders): the
# render callback's rows are the scene rows and the audio row, its emitter mix ahead of the
# scene or page choice, and none of them runs there; the update's rows run on.
func test_the_render_rows_are_the_render_callback() -> void:
	var update_rows := ["begin", "session", "remote_admin", "scene_gate", "network", "finish"]
	for name in Array(GameWorld.frame_leg_names()):
		assert_eq(GameWorld.frame_leg_in_render(name), not update_rows.has(name),
				"%s is the render callback's: %s" % [name, not update_rows.has(name)])
		if GameWorld.frame_leg_draws_scene(name):
			assert_true(GameWorld.frame_leg_in_render(name), "%s is a render row" % name)
	assert_false(GameWorld.frame_leg_draws_scene("audio"), "the audio row runs under the page")
	assert_false(GameWorld.frame_leg_in_render("no_such_leg"))


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
		"water_settle", "particles", "lights", "framefx", "slot_shadows",
		"scene_overlay", "clear",
	])
	# The bloom pass draws the Q3 frame this replay compiles: after the settled
	# glare, stop-down and exposure (the gain its glare copy takes), never the
	# last live frame's snapshot (review C-47).
	for producer in ["celestial_settle", "sun_veil", "weather_settle", "occlusion", "water_settle"]:
		assert_gt(replay.find("framefx"), replay.find(producer),
				"the replay compiles Q3 after %s" % producer)
	var live := Array(GameWorld.frame_leg_names())
	for time_owner in ["session", "environment_nodes", "ragdolls", "materials", "audio", "network", "blink"]:
		assert_true(live.has(time_owner))
		assert_false(replay.has(time_owner),
				"the replay omits the time-owning %s leg" % time_owner)
