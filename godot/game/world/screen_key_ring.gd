class_name ScreenKeyRing
extends RefCounted

## The key ring under a pass-through screen (death.mnu's DEATH, stat.mnu's
## STAT): every key-down waits here, after the screen's hotkey scan took its
## look at it, for the next input frame the screen's word lets run. While the
## word is down (DEATH's exit confirmation) nothing drains it, so the keys
## pressed under the confirmation reach the first input frame after its NO; a
## screen's close resets it with the rest of the input (the shell clears it).
## Escape under either screen therefore never opens the in-game menu: the
## escape row's case finds a menu screen open (stat.mnu's STAT opens through
## UI_OpenMenuScreen like DEATH).
## One drain is the input frame's keyboard pass and then its event drain, in
## the original's order: an open chat line takes each key; otherwise the
## special-key chain runs its inline legs (the quit dialog's keys, the page
## keys) and the deploy leg queues its pick (event 12); a key the chain leaves
## is the binding scan's, where the Escape key queues the escape row's event
## (18). Then the queued events dispatch in order, and the first whose case
## resets the input (12's and 18's Input_ResetAllState memset the event queue)
## drops the rest. So a deploy key pressed under the confirmation sends its
## pick after the NO, ahead of the NO's own Escape, which the pick's reset
## drops; a stale LEAVE MISSION Escape runs the Escape action once.
## (engine world::pass_through_close_axis carries the witness: the key ring,
## the keyboard pass, the event drain and its resets).
##
## The legs are the shell's (MainGame implements them; a test supplies its own):
##   ring_chat_key(key) -> bool        an open chat line took the key
##   ring_special_key(key, events) -> bool
##                                      the special-key chain took the key; its
##                                      deploy leg appends the pick's param
##   ring_escape()                      the Escape action (case 18)
##   ring_pick(param)                   the deploy pick (case 12)
##   ring_reset_ran() -> bool           the case just run reset the input (the
##                                      shell runs the devices' reset)

## The escape row's event in the drain's queue (a pick's param is never negative).
const ESCAPE_EVENT := -1

var _keys: Array[InputEventKey] = []


func push(key: InputEventKey) -> void:
	_keys.append(key)


func clear() -> void:
	_keys.clear()


func size() -> int:
	return _keys.size()


## One input frame over the ring (the class comment carries the order).
func drain(legs: Object) -> void:
	var keys := _keys
	_keys = []
	var events: Array[int] = []
	for key in keys:
		if legs.ring_chat_key(key):
			continue
		if legs.ring_special_key(key, events):
			continue
		if key.keycode == KEY_ESCAPE:
			events.append(ESCAPE_EVENT)
	for event in events:
		if event == ESCAPE_EVENT:
			legs.ring_escape()
		else:
			legs.ring_pick(event)
		if legs.ring_reset_ran():
			return
