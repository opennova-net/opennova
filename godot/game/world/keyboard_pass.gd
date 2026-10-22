class_name KeyboardPass
extends RefCounted

## The input frame's keyboard pass over the device's key queue. Every key-down,
## an OS repeat included, joins the one native queue (KeyEventQueue, which the
## player's input router owns: the 128-slot ring, its wrap and its reset are the
## engine's controls::KeyEventQueue) in every shell state, and each input frame
## drains it: over live play, and under a pass-through screen (death.mnu's DEATH,
## stat.mnu's STAT) while the screen's word holds. Under a menu screen that runs no
## input frame (INGAME, WEAPON, CMAP, VEHICLE, DEATH's exit confirmation) nothing
## drains it, so the keys pressed under the confirmation reach the first input
## frame after its NO; a screen's close resets the queue with the rest of the input
## (the shell resets it), as does every other input reset the shell runs.
## Escape under either pass-through screen therefore never opens the in-game
## menu: the escape row's case finds a menu screen open (stat.mnu's STAT opens
## through UI_OpenMenuScreen like DEATH).
## One drain is the keyboard pass, in the original's order: first the release
## pass (each key whose press latch is set and that reads up queues its release
## row's event, ToSpecial's); then every dequeued key sets its press latch, and an
## open chat line takes it; otherwise the special-key chain runs its inline legs
## (the quit dialog's keys, the digit arms, the round-over leg's, the page keys)
## and the deploy leg queues its pick (event 12; under STAT too, a dead player's);
## a key the chain leaves is the binding scan's, whose row's event it queues, the
## escape row's (18) as any other's. The picks and the row events wait in the
## player input router in key order, and the update's sample turns them into its
## records (a talk, HUD, pause or escape row's and a pick's among the action rows'),
## which the update's drain dispatches in order, the escape row's and the picks
## through the shell's arms (the router's set_record_leg): the first whose case
## resets the input (12's and 18's Input_ResetAllState memset the event queue) ends
## the drain there. The shell's pass runs at the head of each update's input frame
## (the router's run_update_rows), so a frame that runs no update keeps its keys.
## So a deploy key pressed under the confirmation sends its pick after the NO,
## ahead of the NO's own Escape, which the pick's reset drops; a stale LEAVE
## MISSION Escape runs the Escape action once. (Engine
## world::pass_through_close_axis carries the screens' witness; engine
## controls::KeyEventQueue and BindingSet::event_row the queue's and the scan's,
## controls::KeyPressLatch and BindingSet::release_pass the latch's,
## inmatch::Role::drain_input_events the drain's.)
##
## The legs are the shell's (MainGame implements them; a test supplies its own):
##   ring_release_pass()                the release pass, ahead of the dequeue
##   ring_chat_key(key) -> bool        an open chat line took the key
##   ring_special_key(key) -> bool     the special-key chain took the key; its
##                                      event-12 picks (the deploy leg's, a
##                                      co-op round-over RESTART's) are queued
##                                      in key order with the rows' events
##   ring_binding_code(scan_code) -> int
##                                      the binding scan's dispatch code for the
##                                      key (ControlsModel.event_code), -1 none
##   ring_row_event(key, code, scan_code)
##                                      the row's event, the escape row's
##                                      included, for the router

## The escape row's dispatch code (catalog row 67).
const ESCAPE_CODE := 18

var _keys: KeyEventQueue


## Over the given queue (the shell's: its player presenter's), or a queue of its
## own (a test's).
func _init(keys: KeyEventQueue = null) -> void:
	_keys = keys if keys != null else KeyEventQueue.new()


## One key-down into the queue, Ctrl and Shift from the event (the shell feeds the
## router's queue through LocalPlayerPresenter.push_key_event, which reads them
## from the key state).
func push(key: InputEventKey) -> void:
	_keys.push_key(key, key.ctrl_pressed, key.shift_pressed)


## The queue's reset (Input_ResetKeyQueue).
func clear() -> void:
	_keys.reset()


func is_empty() -> bool:
	return _keys.is_empty()


## The next waiting key off the queue, null when none waits (a reader outside the
## input frame that dequeues one key: the start-mission splash's exit test).
func take_key() -> InputEventKey:
	return _keys.pop_key()


## One input frame over the queue (the class comment carries the order).
func drain(legs: Object) -> void:
	legs.ring_release_pass()
	var key := _keys.pop_pass_key()
	while key != null:
		if not legs.ring_chat_key(key) and not legs.ring_special_key(key):
			var scan_code := _keys.last_scan_code()
			var code := int(legs.ring_binding_code(scan_code))
			if code >= 0:
				legs.ring_row_event(key, code, scan_code)
		key = _keys.pop_pass_key()
