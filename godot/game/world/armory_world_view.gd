class_name ArmoryWorldView
extends WorldView

## The armory screen's view of the world: the WorldView pair plus the weapon
## table and the local player's viewmodel verbs the ACCEPT leg drives. The base
## answers none; GameWorld.armory_view() serves the live one; a test fakes it by
## overriding the public verbs (ADR 0043 rule 11).


func weapon_database() -> WeaponDatabase:
	return null


## The equipped weapon's viewmodel definition, null when none is mounted.
func local_player_viewmodel_def() -> PlayerViewmodelDef:
	return null


## Point the FP viewmodel + action FSM at `weapon_name` (the armory apply).
func set_local_player_weapon_by_name(_weapon_name: String,
		_preserve_slot_state: bool = false) -> bool:
	return false


## The authored NONE row: no rendered/action weapon.
func clear_local_player_weapon() -> void:
	pass
