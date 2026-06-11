extends GutTest

const SyncGuardScript = preload("res://engine/ui/sync_guard.gd")


func test_active_is_false_initially() -> void:
	var guard = SyncGuardScript.new()
	assert_false(guard.active, "A fresh SyncGuard should be inactive.")


func test_run_sets_active_during_body_and_clears_after() -> void:
	var guard = SyncGuardScript.new()
	var seen := {"active_inside": false}
	guard.run(func() -> void:
		seen["active_inside"] = guard.active
	)
	assert_true(seen["active_inside"], "active should be true while the body runs.")
	assert_false(guard.active, "active should be cleared after the body returns.")


func test_run_guards_against_reentrant_feedback() -> void:
	# Mirrors the inspector pattern: a field setter no-ops while a sync is pushing
	# fresh model values into controls, so programmatic updates don't echo back.
	var guard = SyncGuardScript.new()
	var writes := {"count": 0}
	var setter := func() -> void:
		if guard.active:
			return
		writes["count"] += 1
	setter.call()  # a real user edit writes through
	guard.run(func() -> void:
		setter.call()  # a sync-driven control update must NOT write back
	)
	assert_eq(writes["count"], 1, "Setter writes once for the user edit and no-ops during sync.")
