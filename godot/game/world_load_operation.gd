class_name WorldLoadOperation
extends RefCounted

## One shell-owned world-load continuation. Cancellation is a request; settled
## is the authoritative edge proving that the awaiting preparation coroutine
## and the bound load Callable have both released their references.

signal cancelled()
signal settled()

enum State { ACTIVE, CANCELLED, SETTLING, SETTLED }

var _state := State.ACTIVE
var _was_cancelled := false


func cancel() -> bool:
	if _state != State.ACTIVE:
		return false
	_state = State.CANCELLED
	_was_cancelled = true
	cancelled.emit()
	return true


func settle() -> void:
	if _state == State.SETTLING or _state == State.SETTLED:
		return
	_state = State.SETTLING
	# Emit from the message queue, after the loader coroutine that called us has
	# returned and released its bound Callable payload.
	call_deferred("_finish_settlement")


func _finish_settlement() -> void:
	if _state != State.SETTLING:
		return
	_state = State.SETTLED
	settled.emit()


func is_cancelled() -> bool:
	return _was_cancelled


func is_settled() -> bool:
	return _state == State.SETTLED


func is_active() -> bool:
	return _state == State.ACTIVE
