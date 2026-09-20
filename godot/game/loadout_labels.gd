class_name LoadoutLabels
extends RefCounted

# The weapon.def load and the gametext WepDes row labels the two loadout
# screens share: the in-game armory (ArmoryMenuCompanion) and the PLAYER_INFO
# loadout panel (PlayerInfoMenuCompanion).


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


## Weapon display name = loadout_menu_textid resolved in gametext's WepDes
## section, else the raw weapon id
## [orig: populate_weapon_slot_lists @ 0x560430: entry+40 textid else entry+0].
static func weapon_label(w: WeaponDef) -> String:
	var textid := w.display_textid
	if textid.is_empty():
		return w.name
	return Strings.lookup_or(Strings.TABLE_GAMETEXT, Strings.SECTION_WEPDES, textid, w.name)


## "<rounds> - <round label>"; a null (unavailable) def rows "<clips> - "
## [orig: sprintf "%d - %s" with i*clipsize + round_type in every ammo fill;
##  the label resolves through gametext WepDes].
static func ammo_row_label(w: WeaponDef, clips: int) -> String:
	if w == null:
		return "%d - " % clips
	var round_label := w.round_type
	if not round_label.is_empty():
		round_label = Strings.lookup_or(Strings.TABLE_GAMETEXT, Strings.SECTION_WEPDES,
				round_label, round_label)
	return "%d - %s" % [clips * w.clipsize, round_label]
