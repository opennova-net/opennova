extends GutTest

const DRIVER_PATH := "res://tests/net/parity_joiner_driver.gd"
const DRIVER := preload(DRIVER_PATH)


func test_parity_joiner_is_frame_driven_and_has_pre_extension_cleanup() -> void:
	assert_true(FileAccess.file_exists(DRIVER_PATH),
			"the verdict-bearing joiner driver is a tracked project resource")
	var source := FileAccess.get_file_as_string(DRIVER_PATH)
	assert_false(source.is_empty())
	var await_pattern := RegEx.new()
	assert_eq(await_pattern.compile("(?m)^(?!\\s*#).*\\bawait\\b"), OK)
	assert_null(await_pattern.search(source),
			"a suspended coroutine must never retain GDExtension objects into teardown")
	assert_false(source.contains("create_timer("),
			"the steady heartbeat stays in the frame-driven MainLoop state machine")
	for required in [
			"func _process(_delta: float) -> bool:",
			"func _finalize() -> void:",
			"_world.call(\"unload\")",
			"const SHUTDOWN_DRAIN_FRAMES := 4",
			"prepare_runtime_shutdown",
			"release_runtime_resources_for_shutdown",
			"_game.free()",
			"NW_LAN_PARITY_STOP_REQUEST_FILE",
			"NW_LAN_PARITY_SHUTDOWN_WITNESS_FILE",
			"opennova.parity-joiner-stop-request.v1",
			"opennova.parity-joiner-shutdown.v1",
	]:
		assert_true(source.contains(required), "driver contract includes %s" % required)
	assert_false(source.contains("PROCESS_MODE_DISABLED"),
			"shutdown keeps the game processing until its suspended load stacks drain")


func test_shutdown_completion_timestamp_is_precise_and_not_before_request() -> void:
	var timestamp_pattern := RegEx.new()
	assert_eq(timestamp_pattern.compile(
			"^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:"
			+ "[0-9]{2}\\.[0-9]{7}Z$"), OK)
	var completed: String = DRIVER.completion_timestamp_for_request(
			"1970-01-01T00:00:00.0000000Z")
	assert_not_null(timestamp_pattern.search(completed),
			"the shutdown witness preserves 100ns-shaped UTC precision")
	var future_request := "2999-12-31T23:59:59.9999999Z"
	assert_eq(DRIVER.completion_timestamp_for_request(future_request), future_request,
			"a backward wall-clock step cannot make completion precede the request")


func test_deploy_hold_witness_publishes_raw_transport_facts() -> void:
	# The witnessed deploy hold (net-re 5.61) is the SPLIT itself: the client
	# enters its protocol InMatch phase and owns a live motor so the pre-pick
	# C2S 0x0C flows, while the player-paced DEATH screen holds presentation.
	# The driver publishes those observations verbatim and the verifier's
	# deploy_hold class (scripts/net/lib.ps1) asserts the combination; a
	# projection helper would let the evidence impersonate observations it
	# never made. Pin the projection's absence and the verbatim publish.
	var source := FileAccess.get_file_as_string(DRIVER_PATH)
	assert_false(source.contains("_project_readiness_evidence_state"),
			"the driver publishes raw witness fields; classification is the verifier's")
	assert_true(source.contains("\"in_match\": bool(state.in_match)"),
			"the witness carries the observed protocol phase verbatim")
	assert_true(source.contains("\"local_player\": bool(state.local_player)"),
			"the witness carries the observed motor presence verbatim")
	assert_true(source.contains("\"joiner_phase\": int(state.joiner_phase)"),
			"the witness carries the observed connection phase verbatim")
