extends GutTest

# The joiner's host-expansion reconcile DECISION (D-NET-178), tested as the pure function it
# is: host expansion x mounted expansion x installed set -> keep / remount / fail. The
# executing half (mount + verify + abort) is covered end to end against a live in-process
# host in godot/tests/net/join_expansion_reconcile_test.gd.


func test_matching_expansion_is_left_alone() -> void:
	var plan := JoinExpansionPlan.decide("jox01", "jox01", PackedStringArray(["jox01"]))
	assert_eq(plan.action, JoinExpansionPlan.ACTION_KEEP)
	assert_eq(plan.error, "")


func test_matching_base_game_is_left_alone() -> void:
	var plan := JoinExpansionPlan.decide("", "", PackedStringArray(["jox01"]))
	assert_eq(plan.action, JoinExpansionPlan.ACTION_KEEP,
		"a base-game host and a base-game mount already agree")


func test_expansion_host_against_a_base_mount_remounts() -> void:
	var plan := JoinExpansionPlan.decide("jox01", "", PackedStringArray(["jox01"]))
	assert_eq(plan.action, JoinExpansionPlan.ACTION_REMOUNT)
	assert_eq(plan.expansion, "jox01")


func test_base_host_against_an_expansion_mount_remounts_to_base() -> void:
	# The direction that is easy to miss: an expansion mounted locally while the host runs
	# base JO misreads the wire exactly as badly as the reverse, because the ADM index space
	# shifts in both directions. Empty is a value here, never "no action".
	var plan := JoinExpansionPlan.decide("", "jox01", PackedStringArray(["jox01"]))
	assert_eq(plan.action, JoinExpansionPlan.ACTION_REMOUNT)
	assert_eq(plan.expansion, "", "base game is the target, and it is always mountable")


func test_uninstalled_host_expansion_fails_and_names_what_is_installed() -> void:
	var plan := JoinExpansionPlan.decide("revx02", "jox01", PackedStringArray(["jox01"]))
	assert_eq(plan.action, JoinExpansionPlan.ACTION_FAIL)
	assert_string_contains(plan.error, "revx02")
	assert_string_contains(plan.error, "jox01")


func test_uninstalled_host_expansion_on_a_base_only_install_says_so() -> void:
	var plan := JoinExpansionPlan.decide("jox01", "", PackedStringArray())
	assert_eq(plan.action, JoinExpansionPlan.ACTION_FAIL)
	assert_string_contains(plan.error, "jox01")
	assert_string_contains(plan.error, "none")


func test_comparisons_are_case_and_whitespace_insensitive() -> void:
	# The host's spelling rides the wire; the installed set carries the local filesystem's.
	assert_eq(JoinExpansionPlan.decide("JOX01", "jox01", PackedStringArray(["jox01"])).action,
		JoinExpansionPlan.ACTION_KEEP)
	var plan := JoinExpansionPlan.decide(" JOX01 ", "", PackedStringArray(["jox01"]))
	assert_eq(plan.action, JoinExpansionPlan.ACTION_REMOUNT)
	assert_eq(plan.expansion, "jox01",
		"the mount uses the ON-DISK spelling, not the wire's")
