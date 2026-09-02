#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <formats/def/def.h>

#include "object/weapon_def.h"

#include <vector>

namespace godot {

class ResourceRoot;

// Thin GDExtension wrapper over engine/formats/def weapon.def parsing (def_parse_weapons), surfacing
// the PLAYER_INFO loadout slice: per-slot weapon lists filtered by the selected class + team,
// with the fields the loadout combos + weight readout consume.
// (engine: base/gameprofile/required_resources.c) (docs/playerinfo/avatars-re.md D-PLAYERINFO-11).
class WeaponDatabase : public RefCounted {
	GDCLASS(WeaponDatabase, RefCounted)

private:
	// The retained weapon.def parse (the ItemDatabase precedent): every getter
	// reads the DefWeaponDef rows directly, the engine's loadout laws take the
	// rows as they are, and nothing re-packs them. Freed at the top of every
	// load attempt and in the destructor.
	DefWeaponsFile weapons_file_ = {};
	bool weapons_file_loaded_ = false;
	String source_path;
	String last_error;

	void release_native_weapons();
	const DefWeaponDef *row(int index) const {
		return (index < 0 || static_cast<size_t>(index) >= weapons_file_.count)
				? nullptr : &weapons_file_.entries[index];
	}

protected:
	static void _bind_methods();

public:
	// weapon_class slot values (engine: formats/def/def.h).
	enum {
		SLOT_ACCESSORY = 0,
		SLOT_PRIMARY = 1,
		SLOT_SECONDARY = 2,
		SLOT_GRENADE = 3,
	};

	// weapon.def flags bits GDScript reads off get_* results — mirrors
	// DEF_WEAPON_FLAG_* (static_assert in the .cpp pins it).
	enum {
		FLAG_EMPLACED = 0x80,
	};

	// flags2 bit: the weapon authors no ammo-type choice — the PLAYER_INFO
	// *_AMMO1_TYPE combo is locked non-interactive and the saved type resets to 0
	// (engine: formats/def/def.cpp).
	enum {
		FLAG2_NOAMMOTYPES = 0x40,
	};

	// Encumbrance bands for loadout weight — mirrors DefEncumbrance
	// (engine: formats/def/def.cpp).
	enum {
		ENCUMBRANCE_LIGHT = 0,
		ENCUMBRANCE_NORMAL = 1,
		ENCUMBRANCE_HEAVY = 2,
	};

	// The untouched-ammo sentinel: retail serializes -1 until the user picks a
	// clip row, and every default-select/weight leg keys on it
	// (engine: net/npruntime/loadout_submit.cpp).
	enum {
		CLIP_COUNT_DEF_DEFAULT = -1,
	};

	// The armory class-allow / class-filter mask domains — mirrors
	// engine/runtime/world player_loadout.h (static_asserts in the .cpp pin
	// them): the no-restriction S2C 0x76 allow mask (wire class bits 0..9)
	// and the five-class charfilter union ("show every class's weapons").
	enum {
		CLASS_ALLOW_ALL = 0x3FF,
		CLASS_MASK_ALL = 0x1F,
	};

	~WeaponDatabase() override;

	Error load(const String &path);
	// Load weapon.def by flat name through the mounted resource root (VFS / PFF).
	Error load_from_resource_root(const Ref<ResourceRoot> &p_resource_root, const String &p_name);
	bool is_loaded() const;
	String get_source_path() const;
	String get_last_error() const;
	int get_count() const;

	// The weapons that belong in `slot` for the given class + team masks, in table order.
	// The filter is the engine's world::weapon_slot_indices
	// (engine: formats/def/def.h). Each entry is a WeaponDef record; the
	// caller prepends the "NONE" row.
	TypedArray<WeaponDef> get_slot_weapons(int slot, int class_mask, int team_mask) const;
	// One weapon by table index as a WeaponDef record; null out of range.
	Ref<WeaponDef> get_weapon(int index) const;
	// Table index of the weapon named `name` (the raw weapon "<id>" token,
	// case-insensitive like every def lookup), or -1 when absent.
	int find_weapon(const String &name) const;

	// Total loadout weight over the indexed weapons: per entry weaponweight +
	// (count <= 0 ? maxclips : count) * clipweight — the engine/formats/def port of the
	// parent-slot terms (engine: formats/def/def.cpp). Invalid
	// indices contribute nothing; a short counts array reads as -1 (default).
	double loadout_weight(const PackedInt32Array &weapon_indices,
			const PackedInt32Array &ammo_counts) const;
	// One extra-ammo (category-3) term for the indexed weapon: count *
	// clipweight only, CLIP_COUNT_DEF_DEFAULT -> the maxclips default, a
	// chosen zero row weighs nothing (engine/formats/def def_extra_ammo_weight
	// (engine: formats/def/def.cpp)).
	double extra_ammo_weight(int p_index, int p_count) const;
	// The sub-weapon behind a parent slot's *_AMMO2: the absolute index of the
	// first differing-round_type entry in the parent's loadout_subclasses
	// window, or -1 (engine/formats/def def_subclass_weapon_index
	// (engine: formats/def/def.cpp)).
	int subclass_weapon_index(int p_parent_index) const;
	// The encumbrance band for a weight (ENCUMBRANCE_*)
	// (engine: formats/def/def.cpp).
	int encumbrance_class(double weight) const;
	// The PLAYER_INFO screen policies (one impl in engine/runtime/world
	// player_loadout.h (engine: runtime/world/player_loadout.cpp)): the team mask (team 0 -> 2, else
	// 1), the class mask (5..9 -> its bit, else nothing), and the ammo combo's
	// default-select clip count (saved > 0 clamped into 1..maxclips, the
	// CLIP_COUNT_DEF_DEFAULT sentinel -> the full maxclips row).
	static int player_info_team_mask(int p_team);
	static int player_info_class_mask(int p_playerclass_value);
	static int default_clip_row(int p_saved, int p_maxclips);
	// The armory screen's open-time class policy (one impl in
	// engine/runtime/world player_loadout.h (engine: runtime/world/player_loadout.cpp)): the current class when the S2C 0x76 allow mask permits it,
	// else scan up through 9, else gunner (7); and the class filter bit
	// (1 << (class-5) for 5..9, ALL weapons otherwise).
	static int armory_resolve_selected_class(int p_player_class, int p_class_allow_mask);
	static int armory_class_filter_mask(int p_selected_class);
	// The armory PLAYER_CLASS spin catalog, authored order — ArmoryClassRow
	// records (one table in engine/runtime/world player_loadout.h
	// kArmoryClassCatalog, which carries the UI_InitWeaponClassSelection
	// witness — CHARCLASS_MEDIC..ENGINEER, 5..9).
	static TypedArray<ArmoryClassRow> armory_class_catalog();
};

} // namespace godot
