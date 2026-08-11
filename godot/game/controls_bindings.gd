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
		var blob: Dictionary = ConfigStore.read(CONFIG_PATH, SECTION, KEY, {})
		if not blob.is_empty():
			_model.load_blob(blob)
	return _model


static func persist() -> void:
	if _model != null:
		ConfigStore.write(CONFIG_PATH, SECTION, KEY, _model.save_blob())


## Whether any key bound to the catalog token is held (primary or secondary).
static func pressed(token: String) -> bool:
	for key in model().godot_keys_for_token(token):
		if Input.is_physical_key_pressed(key):
			return true
	return false
