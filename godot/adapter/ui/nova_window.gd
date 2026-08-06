class_name NovaWindow
extends RefCounted

## Fullscreen as a core engine concept: one shell-neutral helper every surface
## routes through — the game shell's and ONED's F11 key, the editor MCP's
## set_fullscreen tool, and scripted capture drivers — so the behavior (and any
## future window-state policy) lives in exactly one place. F11 is the canonical
## binding; owners recognize it via is_toggle_event in their key handlers.

const TOGGLE_KEY := KEY_F11


static func is_fullscreen(window: Window) -> bool:
	if window == null:
		return false
	return window.mode == Window.MODE_FULLSCREEN \
			or window.mode == Window.MODE_EXCLUSIVE_FULLSCREEN


static func set_fullscreen(window: Window, enabled: bool) -> void:
	if window == null or enabled == is_fullscreen(window):
		return
	window.mode = Window.MODE_FULLSCREEN if enabled else Window.MODE_WINDOWED


## Returns the resulting state.
static func toggle_fullscreen(window: Window) -> bool:
	set_fullscreen(window, not is_fullscreen(window))
	return is_fullscreen(window)


## True when `event` is the canonical fullscreen key edge (F11 pressed, no echo).
static func is_toggle_event(event: InputEvent) -> bool:
	var key := event as InputEventKey
	return key != null and key.pressed and not key.echo and key.keycode == TOGGLE_KEY
