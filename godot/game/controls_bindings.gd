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
## The records blob's schema stamp. A blob applies only under this build's
## stamp: the cfg is the pre-profile stand-in, and a file an earlier build
## wrote (before the catalog defaults carried the seat rows' Ctrl modifier)
## would pin those rows to bare digits over the seeded defaults. Pre-1.0 there
## is no migration: an unstamped or mismatched blob is dropped.
const SCHEMA_KEY := "schema"
const SCHEMA := 2

static var _model: ControlsModel = null


static func model() -> ControlsModel:
	if _model == null:
		_model = ControlsModel.new()
		load_saved_records(_model)
	return _model


## Apply the persisted records to `into` when the file carries a Dictionary
## blob under the current schema stamp; returns whether it did. Anything else
## (no file, a corrupt Variant at either key, a stale stamp) leaves the catalog
## defaults in place.
static func load_saved_records(into: ControlsModel) -> bool:
	var stamp: Variant = ConfigStore.read(CONFIG_PATH, SECTION, SCHEMA_KEY, 0)
	if not (stamp is int) or int(stamp) != SCHEMA:
		return false
	var blob: Variant = ConfigStore.read(CONFIG_PATH, SECTION, KEY, {})
	if not (blob is Dictionary) or (blob as Dictionary).is_empty():
		return false
	into.load_blob(blob)
	return true


static func persist() -> void:
	if _model == null:
		return
	var blob: Dictionary = _model.save_blob()
	ConfigStore.update(CONFIG_PATH, func(config: ConfigFile) -> void:
		config.set_value(SECTION, SCHEMA_KEY, SCHEMA)
		config.set_value(SECTION, KEY, blob))


## Whether the token's binding is held: keyboard slots (Ctrl-/Shift- combos
## gate on their modifier) plus held mouse buttons — one seam call.
static func pressed(token: String) -> bool:
	return model().is_token_pressed(token)
