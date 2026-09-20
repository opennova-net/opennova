class_name LoadoutLabels
extends RefCounted

# The weapon.def load the two loadout screens share: the in-game armory
# (ArmoryMenuCompanion) and the PLAYER_INFO loadout panel
# (PlayerInfoMenuCompanion). The row labels, the row order and the weight
# readout are the engine's (WeaponDatabase over runtime/menu/loadout_labels.h).


## weapon.def off the root, or null with a warning naming the owner and what
## stays empty (best-effort: an absent table leaves the slot lists empty).
static func load_weapon_database(root: ResourceRoot, owner_name: String,
		consequence: String) -> WeaponDatabase:
	var weapons := WeaponDatabase.new()
	if weapons.load_from_resource_root(root, "weapon.def") != OK or not weapons.is_loaded():
		push_warning("%s: weapon.def not loaded (%s); %s"
			% [owner_name, weapons.get_last_error(), consequence])
		return null
	return weapons
