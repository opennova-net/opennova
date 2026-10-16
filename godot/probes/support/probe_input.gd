class_name ProbeInput
extends RefCounted

## Synthetic input the manual probes share: parsed through Input so the
## shell's real action map and presenters see it exactly like a device.


## Press (down = true) or release a key by keycode + physical keycode.
static func hold(k: Key, down: bool) -> void:
	var e := InputEventKey.new()
	e.keycode = k
	e.physical_keycode = k
	e.pressed = down
	Input.parse_input_event(e)


## A full key tap: the press and its release through the input stack.
static func tap(k: Key) -> void:
	hold(k, true)
	hold(k, false)


## The live `useitem` row's first key (retail default Shift; rebindable in
## Options -> Controls): USE, the armory opener and, while the WEAPON screen is
## up, its ACCEPT accelerator (ArmoryPresenter.is_accept_hotkey). KEY_NONE when
## the row has no key.
static func use_item_key() -> Key:
	var keys := ControlsBindings.model().godot_keys_for_token("useitem")
	return (keys[0] as Key) if not keys.is_empty() else KEY_NONE


## Press or release a mouse button.
static func mouse_btn(b: MouseButton, down: bool) -> void:
	var e := InputEventMouseButton.new()
	e.button_index = b
	e.pressed = down
	Input.parse_input_event(e)


## A mouse look of `total` pixels delivered as ten equal relative motions.
static func look(total: Vector2) -> void:
	for _i in 10:
		var mm := InputEventMouseMotion.new()
		mm.relative = total / 10.0
		Input.parse_input_event(mm)
