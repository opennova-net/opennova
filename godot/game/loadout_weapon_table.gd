class_name LoadoutWeaponTable
extends RefCounted

# The weapon.def load the two loadout screens share: the in-game armory
# (ArmoryMenuCompanion) and the PLAYER_INFO loadout panel
# (PlayerInfoMenuCompanion). The row labels, the row order and the weight
# readout are the engine's (WeaponDatabase over runtime/menu/loadout_labels.h).


## weapon.def off the root. A missing or empty file is a loaded, empty table
## (each list then shows only its NONE row, D-MNU-27); null, with a warning
## naming the owner and what stays empty, only when the root is unusable.
static func load_weapon_database(root: ResourceRoot, owner_name: String,
		consequence: String) -> WeaponDatabase:
	var weapons := WeaponDatabase.new()
	if weapons.load_from_resource_root(root, "weapon.def") != OK or not weapons.is_loaded():
		push_warning("%s: weapon.def not loaded (%s); %s"
			% [owner_name, weapons.get_last_error(), consequence])
		return null
	return weapons


## The mount a table was read from: the root object, its folder and the
## expansion it mounted ("" with no root). CHANGE FOLDER hands over a new
## root and a Mods switch remounts the same one under another expansion, so
## either changes the key and a screen that outlives the mount reads
## weapon.def again, as the game reloads its catalog at the switch
## [orig: Game_ReloadExpansionAndMods @0x552710, inline @0x55282b..0x55285b].
static func mount_key(root: ResourceRoot) -> String:
	if root == null:
		return ""
	return "%d|%s|%s" % [root.get_instance_id(), String(root.get_root_dir()),
			String(root.get_expansion()).to_lower()]
