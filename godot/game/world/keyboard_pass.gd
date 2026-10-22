class_name KeyboardPass
extends RefCounted

## The input frame's keyboard pass over the device's key queue, then its event
## drain. Every key-down, an OS repeat included, joins the one native queue
## (KeyEventQueue, which the player's input router owns: the 128-slot ring, its
## wrap and its reset are the engine's controls::KeyEventQueue) in every shell
## state, and each input frame drains it: over live play, and under a
## pass-through screen (death.mnu's DEATH, stat.mnu's STAT) while the screen's
## word holds. Under a menu screen that runs no input frame (INGAME, WEAPON,
## CMAP, VEHICLE, DEATH's exit confirmation) nothing drains it, so the keys
## pressed under the confirmation reach the first input frame after its NO; a
## screen's close resets the queue with the rest of the input (the shell resets
## it), as does every other input reset the shell runs.
## Escape under either pass-through screen therefore never opens the in-game
## menu: the escape row's case finds a menu screen open (stat.mnu's STAT opens
## through UI_OpenMenuScreen like DEATH).
## One drain is the keyboard pass and then the event drain, in the original's
## order: first the release pass (each key whose press latch is set and that reads
## up queues its release row's event, ToSpecial's); then every dequeued key sets
## its press latch, and an open chat line takes it; otherwise the special-key chain
## runs its inline legs (the quit dialog's keys, the round-over leg's, the page
## keys) and the deploy leg queues its pick (event 12; under STAT too, a dead
## player's); a key the chain leaves is the binding scan's: the escape row's key
## queues its event (18), any other row's key its row event, which the player
## input router dispatches once per event in the same update (a talk or HUD row's
## behind the pass in its input frame, an action row's in its sample). The shell's
## pass runs at the head of each update's input frame (the router's
## run_update_rows), so a frame that runs no update keeps its keys. Then the
## queued events dispatch in order,
## and the first whose case resets the input (12's and 18's Input_ResetAllState
## memset the event queue) drops the rest. So a deploy key pressed under the
## confirmation sends its pick after the NO, ahead of the NO's own Escape, which
## the pick's reset drops; a stale LEAVE MISSION Escape runs the Escape action
## once. (Engine world::pass_through_close_axis carries the screens' witness;
## engine controls::KeyEventQueue and BindingSet::event_row the queue's and the
## scan's, controls::KeyPressLatch and BindingSet::release_pass the latch's.)
##
## The legs are the shell's (MainGame implements them; a test supplies its own):
##   ring_release_pass()                the release pass, ahead of the dequeue
##   ring_chat_key(key) -> bool        an open chat line took the key
##   ring_special_key(key, events) -> bool
##                                      the special-key chain took the key; its
##                                      event-12 picks (the deploy leg's, a
##                                      co-op round-over RESTART's) append
##                                      their params
##   ring_binding_code(scan_code) -> int
##                                      the binding scan's dispatch code for the
##                                      key (ControlsModel.event_code), -1 none
##   ring_row_event(key, code, scan_code)
##                                      any other row's event, for the router
##   ring_escape()                      the Escape action (case 18)
##   ring_pick(param)                   the deploy pick (case 12)
##   ring_reset_ran() -> bool           the case just run reset the input (the
##                                      shell runs the devices' reset)

## The escape row's dispatch code (catalog row 67).
const ESCAPE_CODE := 18
## The escape row's event in the drain's queue (a pick's param is never negative).
const ESCAPE_EVENT := -1

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
	var events: Array[int] = []
	var key := _keys.pop_pass_key()
	while key != null:
		if not legs.ring_chat_key(key) and not legs.ring_special_key(key, events):
			var scan_code := _keys.last_scan_code()
			var code := int(legs.ring_binding_code(scan_code))
			if code == ESCAPE_CODE:
				events.append(ESCAPE_EVENT)
			elif code >= 0:
				legs.ring_row_event(key, code, scan_code)
		key = _keys.pop_pass_key()
	for event in events:
		if event == ESCAPE_EVENT:
			legs.ring_escape()
		else:
			legs.ring_pick(event)
		if legs.ring_reset_ran():
			return
