extends GutTest

const SignalRebindScript = preload("res://modtools/framework/signal_rebind.gd")


class Emitter:
	extends RefCounted
	signal changed(value)


class SignalLess:
	extends RefCounted
	# Deliberately has no "changed" signal.


var _hits: int = 0
var _last: Variant = null


func before_each() -> void:
	_hits = 0
	_last = null


func _on_changed(value = null) -> void:
	_hits += 1
	_last = value


func test_rebind_connects_to_new_source() -> void:
	var emitter := Emitter.new()
	SignalRebindScript.rebind(null, emitter, &"changed", _on_changed)
	assert_true(emitter.changed.is_connected(_on_changed), "rebind should connect the callable to the new source.")
	emitter.changed.emit(7)
	assert_eq(_hits, 1, "The connected callable should fire once.")
	assert_eq(_last, 7, "The callable should receive the emitted value.")


func test_rebind_moves_subscription_off_old_source() -> void:
	var old_source := Emitter.new()
	var new_source := Emitter.new()
	SignalRebindScript.rebind(null, old_source, &"changed", _on_changed)
	SignalRebindScript.rebind(old_source, new_source, &"changed", _on_changed)
	assert_false(old_source.changed.is_connected(_on_changed), "rebind should disconnect from the old source.")
	assert_true(new_source.changed.is_connected(_on_changed), "rebind should connect to the new source.")
	old_source.changed.emit(1)
	assert_eq(_hits, 0, "The old source should no longer reach the callable.")
	new_source.changed.emit(2)
	assert_eq(_hits, 1, "Only the new source should reach the callable.")


func test_rebind_is_idempotent() -> void:
	var emitter := Emitter.new()
	SignalRebindScript.rebind(null, emitter, &"changed", _on_changed)
	SignalRebindScript.rebind(null, emitter, &"changed", _on_changed)
	assert_eq(emitter.changed.get_connections().size(), 1, "rebind should not create duplicate connections.")
	emitter.changed.emit(0)
	assert_eq(_hits, 1, "An idempotent rebind should still fire only once.")


func test_rebind_tolerates_sources_without_the_signal() -> void:
	var signal_less := SignalLess.new()
	# Neither of these should raise: missing-signal sources are simply skipped.
	SignalRebindScript.rebind(null, signal_less, &"changed", _on_changed)
	SignalRebindScript.rebind(signal_less, null, &"changed", _on_changed)
	assert_eq(_hits, 0, "A source without the signal should not be connected.")


func test_rebind_passes_connect_flags() -> void:
	var emitter := Emitter.new()
	SignalRebindScript.rebind(null, emitter, &"changed", _on_changed, CONNECT_DEFERRED)
	var conns := emitter.changed.get_connections()
	assert_eq(conns.size(), 1, "Should connect exactly once.")
	assert_eq(int(conns[0]["flags"]) & CONNECT_DEFERRED, CONNECT_DEFERRED, "The CONNECT_DEFERRED flag should be applied.")
