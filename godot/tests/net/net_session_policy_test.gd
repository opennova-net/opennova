extends GutTest

# The NetSessionPolicy binding contract — the joiner session-drive policy
# SessionDrive executes (engine/runtime/inmatch join_session_policy). The
# machine itself is pinned by the npruntime_join_session_policy ctest; these
# pins cover the binding round trip on the surfaces the drive consumes: the
# D-NET-178 expansion reconcile decision (host x mounted x installed), the
# preload window, and one admission frame walked edge to edge. The executing
# half (mount + verify + abort) is covered end to end against a live
# in-process host in godot/tests/net/join_expansion_reconcile_test.gd.


func test_matching_expansion_is_left_alone() -> void:
	var policy := NetSessionPolicy.new()
	assert_eq(policy.decide_expansion("jox01", "jox01", PackedStringArray(["jox01"])),
		NetSessionPolicy.ACTION_KEEP)
	assert_eq(policy.decision_error(), "")


func test_expansion_host_against_a_base_mount_remounts() -> void:
	var policy := NetSessionPolicy.new()
	assert_eq(policy.decide_expansion("jox01", "", PackedStringArray(["jox01"])),
		NetSessionPolicy.ACTION_REMOUNT)
	assert_eq(policy.decided_expansion(), "jox01")


func test_base_host_against_an_expansion_mount_remounts_to_base() -> void:
	# The direction that is easy to miss: an expansion mounted locally while the host runs
	# base JO misreads the wire exactly as badly as the reverse, because the ADM index space
	# shifts in both directions. Empty is a value here, never "no action".
	var policy := NetSessionPolicy.new()
	assert_eq(policy.decide_expansion("", "jox01", PackedStringArray(["jox01"])),
		NetSessionPolicy.ACTION_REMOUNT)
	assert_eq(policy.decided_expansion(), "", "base game is the target, and it is always mountable")


func test_uninstalled_host_expansion_fails_and_names_what_is_installed() -> void:
	var policy := NetSessionPolicy.new()
	assert_eq(policy.decide_expansion("revx02", "jox01", PackedStringArray(["jox01"])),
		NetSessionPolicy.ACTION_FAIL)
	assert_string_contains(policy.decision_error(), "revx02")
	assert_string_contains(policy.decision_error(), "jox01")
	var base_only := NetSessionPolicy.new()
	base_only.decide_expansion("jox01", "", PackedStringArray())
	assert_string_contains(base_only.decision_error(), "none",
		"a base-only install says nothing is installed")


func test_comparisons_are_case_and_whitespace_insensitive() -> void:
	# The host's spelling rides the wire; the installed set carries the local filesystem's.
	var policy := NetSessionPolicy.new()
	assert_eq(policy.decide_expansion("JOX01", "jox01", PackedStringArray(["jox01"])),
		NetSessionPolicy.ACTION_KEEP)
	assert_eq(policy.decide_expansion(" JOX01 ", "", PackedStringArray(["jox01"])),
		NetSessionPolicy.ACTION_REMOUNT)
	assert_eq(policy.decided_expansion(), "jox01",
		"the mount uses the ON-DISK spelling, not the wire's")


func test_preload_window_is_the_retail_connect_window() -> void:
	var policy := NetSessionPolicy.new()
	policy.arm_preload(1000)
	assert_eq(policy.preload_step("", 1000), NetSessionPolicy.STEP_WAIT)
	assert_eq(policy.preload_step("", 1000 + 59999), NetSessionPolicy.STEP_WAIT,
		"one ms before the 0xEA60 window closes it still waits")
	assert_eq(policy.preload_step("", 1000 + 60000), NetSessionPolicy.STEP_FAIL,
		"the retail ConnectOrHost window closes the preload")
	assert_string_contains(policy.fail_reason(), "timed out")
	policy.arm_preload(1000)
	assert_eq(policy.preload_step("no route", 1001), NetSessionPolicy.STEP_FAIL)
	assert_eq(policy.fail_reason(), "join failed: no route")


func test_promote_validation_normalizes_the_wire_basename() -> void:
	var policy := NetSessionPolicy.new()
	assert_false(policy.validate_promote_mission_file("   "))
	assert_string_contains(policy.fail_reason(), "empty map_file")
	assert_true(policy.validate_promote_mission_file(" AShi5A "))
	assert_eq(policy.promoted_mission_file(), "AShi5A.bms")
	assert_true(policy.validate_promote_mission_file("mnml.BMS"))
	assert_eq(policy.promoted_mission_file(), "mnml.BMS",
		"an existing extension is matched case-insensitively and kept verbatim")
	assert_false(policy.validate_promote_header(615))
	assert_string_contains(policy.fail_reason(), "616-byte")
	assert_true(policy.validate_promote_header(616))


func test_admission_frame_walks_deploy_then_ready_edges() -> void:
	var policy := NetSessionPolicy.new()
	policy.arm_admission_watch(0)
	# A pending deploy pick: settle owed, the rising edge emits once, the
	# watchdog disarms (the DEATH screen is the player-paced hold).
	var pre: int = policy.begin_admission_frame("", true, false, "", "", 1)
	assert_true((pre & NetSessionPolicy.SETTLE_REQUIRED) != 0)
	var post: int = policy.finish_admission_frame(true, false)
	assert_true((post & NetSessionPolicy.EMIT_DEPLOY_PICK) != 0)
	assert_false(policy.is_admission_watch_active())
	policy.begin_admission_frame("", true, false, "", "", 2)
	assert_eq(policy.finish_admission_frame(true, false), 0,
		"a held pick does not re-emit")
	# The pick releases, the cold wire drain empties: admission-ready once.
	policy.begin_admission_frame("", false, true, "", "", 3)
	post = policy.finish_admission_frame(true, true)
	assert_true((post & NetSessionPolicy.EMIT_ADMISSION_READY) != 0)
	policy.begin_admission_frame("", false, true, "", "", 4)
	assert_eq(policy.finish_admission_frame(true, true), 0,
		"admission-ready is once per join")


func test_session_loss_latches_once_and_rearms_per_join() -> void:
	var policy := NetSessionPolicy.new()
	policy.arm_admission_watch(0)
	var pre: int = policy.begin_admission_frame("host punt", true, true, "", "", 1)
	assert_true((pre & NetSessionPolicy.EMIT_SESSION_LOST) != 0,
		"terminal state wins over every admission edge")
	assert_true((pre & NetSessionPolicy.FRAME_DONE) != 0)
	assert_eq(policy.session_loss_reason(), "host punt")
	assert_false(policy.is_admission_watch_active())
	pre = policy.begin_admission_frame("host punt", true, true, "", "", 2)
	assert_eq(pre, NetSessionPolicy.FRAME_DONE,
		"one session-loss notification per session; later frames only suppress")
	policy.reset_for_join()
	pre = policy.begin_admission_frame("host punt", false, false, "", "", 3)
	assert_true((pre & NetSessionPolicy.EMIT_SESSION_LOST) != 0,
		"a fresh join attempt re-arms the notification")
