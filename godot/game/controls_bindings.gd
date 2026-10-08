class_name ControlsBindings
extends RefCounted

## The one LIVE key-binding model: the game's catalog rows, which the gameplay
## input sampler and the HUD's key labels read. The player profile's binding
## table reaches it at every session start and at the in-game options Accept
## (apply_profile, engine profile::apply_controls); the Options screens edit
## their own copy of the table (engine menu::OptionsScreen), which their
## ACCEPT stores into the profile. Until the first session start it holds the
## catalog defaults, as the original's rows do until its first controls apply
## (engine profile::apply_controls).

static var _model: ControlsModel = null


static func model() -> ControlsModel:
	if _model == null:
		_model = ControlsModel.new()
	return _model


## The controls apply over the profile's current record: its binding table
## onto the live rows and its ENABLE_JOYSTICK word into the joystick gate.
static func apply_profile(profiles: PlayerProfiles) -> void:
	model().apply_profile(profiles)


## Whether the token's binding is held: keyboard slots (Ctrl-/Shift- combos
## gate on their modifier) plus held mouse buttons — one seam call.
static func pressed(token: String) -> bool:
	return model().is_token_pressed(token)
