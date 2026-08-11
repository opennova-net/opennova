class_name ControlsBindings
extends RefCounted

## The one LIVE key-binding model, shared by the menu shell (the Options ->
## Controls remap table) and the gameplay input sampler, plus its user://
## persistence. The record semantics live in engine/runtime/controls
## (binding_set.h carries the [orig:] chain).
##
## Retail persists the records inside the player profile (player.sav record
## +1804/+1808 [orig: PlayerProfile_SaveToFiles @ 0x54be00; the profile copy
## @ 0x559d50]); the player-profile format slice is not written yet, so the
## records ride a settings ConfigFile until it is (ledger D-CTRL-3).

const CONFIG_PATH := "user://controls.cfg"
const SECTION := "bindings"
const KEY := "records"

static var _model: ControlsModel = null


static func model() -> ControlsModel:
	if _model == null:
		_model = ControlsModel.new()
		# A corrupt cfg can hold any Variant at this key: only a Dictionary
		# blob loads, anything else falls back to the defaults.
		var blob: Variant = ConfigStore.read(CONFIG_PATH, SECTION, KEY, {})
		if blob is Dictionary and not (blob as Dictionary).is_empty():
			_model.load_blob(blob)
	return _model


static func persist() -> void:
	if _model != null:
		ConfigStore.write(CONFIG_PATH, SECTION, KEY, _model.save_blob())


## Whether the token's binding is held: keyboard slots (Ctrl-/Shift- combos
## gate on their modifier) plus held mouse buttons — one seam call.
static func pressed(token: String) -> bool:
	return model().is_token_pressed(token)
