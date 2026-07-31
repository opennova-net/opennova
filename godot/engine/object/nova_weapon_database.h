#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include <vector>

struct DefWeaponDef;

namespace godot {

class NovaResourceRoot;

// Thin GDExtension wrapper over libs/def weapon.def parsing (def_parse_weapons), surfacing
// the PLAYER_INFO loadout slice: per-slot weapon lists filtered by the selected class + team,
// with the fields the loadout combos + weight readout consume.
// [orig: WeaponDef_LoadAll @ 0x54dd10 / WeaponDef_ParseProperty @ 0x54d730; consumer
//  populate_weapon_slot_lists @ 0x560430] (docs/playerinfo/avatars-re.md D-PLAYERINFO-11).
class NovaWeaponDatabase : public RefCounted {
	GDCLASS(NovaWeaponDatabase, RefCounted)

private:
	struct Weapon {
		String name;           // weapon "<id>" — raw id, the display fallback
		String display_textid; // loadout_menu_textid — GameText "WepDes" key
		String round_type;     // ammo key (GameText "WepDes")
		String icon;           // loadout_menu_icon
		int selectable = 0;    // loadout_selectable (gate)
		int loadout_subclasses = 0; // sub-entry expansion count (+36) — the *_AMMO2 walk bound
		int slot = 0;          // weapon_class: 0=accessory 1=primary 2=secondary 3=grenade
		int team_mask = 0;     // teamfilter: blue/yellow=2, red/violet=1
		int class_mask = 0;    // charfilter: medic1 sniper2 gunner4 rifleman8 engineer16
		float weight = 0.0f;   // weaponweight
		float clip_weight = 0.0f;
		int clipsize = 0;
		int startrounds = 0;
		int maxclips = 0;
		// First-person viewmodel slice [orig: WeaponDef_ParseProperty @ 0x54d730 rows;
		// consumer Player_RenderFirstPersonViewModel @ 0x4ded60]: the FP gun model (gfx1),
		// the character arms riding its skeleton (gfx1a, alternate skin gfx1b), the 3P
		// model (gfx3), the shared animation set (animadm), the hip/ADS view biases
		// (pos/tpos: xyz raw file units + yaw/pitch/roll degrees), and renderfov
		// (horizontal degrees, record default 80.0 — no shipped JO def sets it).
		String animadm;
		String gfx1;
		String gfx1a;
		String gfx1b;
		String gfx3;
		float pos[6] = { 0, 0, 0, 0, 0, 0 };
		float tpos[6] = { 0, 0, 0, 0, 0, 0 };
		float renderfov = 80.0f;
		// The witnessed WeaponDef+8 flag mask (libs/def flag_table maps the file
		// tokens: scoped 1, sighted 2, burst 0x20, auto 0x100, ...) and the ADS zoom
		// magnification [orig: Player_ToggleWeaponScope @ 0x4df0c0 gates Flags & 3;
		// scoped FOV = 80 / zoom @ 0x4df401].
		int flags = 0;
		int flags2 = 0;  // the second FLAGS dword (Inset 0x200 = the 7-step ADS ease)
		float scope_max_mag = 0.0f;
		// The SIGHTS card rows (dicts: texture/x1/y1/x2/y2/blend/scale/slide/
		// slide_frames), authored order [orig: record +0x1C8, count +0x258].
		Array sights;
		// The 3P body-channel kinds (0 = absent, rifle): special_hold 1..8 picks the
		// body hold-pose ladder 50-61 (2 also selects reload2), attack_anim 1/2 stamps
		// 62/63 on fire [orig: AdmDefs +0xA4/+0xA8, read @ 0x4b5dba / @ 0x542bbc].
		int special_hold = 0;
		int attack_anim = 0;
		// The heat model, in the def's pre-divided 16.16 units (0 = the weapon
		// authors no heat, which is every infantry weapon in the shipped corpora)
		// [orig: WeaponDef +0x36C / +0x370 / +0x374 / +0x358].
		int heat_per_shot = 0;
		int heat_decay_per_tick = 0;
		int heat_glow_threshold = 0;
		String heat_effect;
		// Run-gait class: the forward-walk promotion adds this to the constant pitch
		// tier 2 to pick run_2/run_3 [orig: 'run_anim' -> AdmDefs +0xAC; @ 0x4b729d].
		int run_anim = 0;
		// HUD weapon-coupled slice (docs/interface/hud-re.md): the 6-row dispersion
		// table in DEGREES, rows = hip prone/crouch/stand then scoped prone/crouch/
		// stand — the crosshair spread reads ERROR[stance + 3*scoped] [orig: weapon
		// +0xB0 parse @0x543b21 (16.16); HUD_DrawCrosshair @0x592b84]. The clip
		// graphic (HUDCLIPGFX: offset + texture [orig: parse @0x54427f]) and the
		// per-round row (HUDRNDGFX: start x/y, step x/y, rounds-per-icon divisor,
		// texture [orig: parse @0x5442fc -> weapon +644/+648/+652/+656/+727]).
		float error[6] = { 0, 0, 0, 0, 0, 0 };
		String hudclipgfx_texture;
		int hudclipgfx_offset[2] = { 0, 0 };
		String hudrndgfx_texture;
		int hudrndgfx_offset[2] = { 0, 0 };
		int hudrndgfx_layout[3] = { 0, 0, 0 }; // step_x, step_y, rounds-per-icon
		// The weapon's ACTION blocks, verbatim rows for the weapon-FSM bake — Dicts
		// {name, anim, function, delaystart, delayend} [orig: ActionDef_ParseScriptLine
		// @ 0x4023c0; bound by Anim_InitActions @ 0x541fa0; net-re §5.62].
		Array actions;
	};
	std::vector<Weapon> weapons; // file order (mirrors the engine's table order)
	String source_path;
	String last_error;

	Dictionary weapon_dict(int index) const;
	void append_entry(const DefWeaponDef &e);

protected:
	static void _bind_methods();

public:
	// weapon_class slot values [orig: WeaponDef_ParseProperty @ 0x54d730 +108].
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
	// [orig: populate_ammo_combo_boxes @ 0x55def0 gates on +188 & 0x40].
	enum {
		FLAG2_NOAMMOTYPES = 0x40,
	};

	// Encumbrance bands for loadout weight — mirrors DefEncumbrance
	// [orig: update_player_info_weight_and_weapon_icons @ 0x55f480:
	//  >= 66.6 HEAVY, >= 33.3 NORMAL, else LIGHT].
	enum {
		ENCUMBRANCE_LIGHT = 0,
		ENCUMBRANCE_NORMAL = 1,
		ENCUMBRANCE_HEAVY = 2,
	};

	Error load(const String &path);
	// Load weapon.def by flat name through the mounted resource root (VFS / PFF).
	Error load_from_resource_root(const Ref<NovaResourceRoot> &p_resource_root, const String &p_name);
	bool is_loaded() const;
	String get_source_path() const;
	String get_last_error() const;
	int get_count() const;

	// The weapons that belong in `slot` for the given class + team masks, in table order.
	// Faithful filter [orig: populate_weapon_slot_lists @ 0x560430]: a row is included only
	// when selectable != 0 AND (class_mask & player_class_mask) AND (team_mask & player_team_mask).
	// Each entry is a weapon_dict(); the caller prepends the "NONE" row.
	Array get_slot_weapons(int slot, int class_mask, int team_mask) const;
	// All weapons, unfiltered, in table order.
	Array get_weapons() const;
	Dictionary get_weapon(int index) const;
	// Table index of the weapon named `name` (the raw weapon "<id>" token,
	// case-insensitive like every def lookup), or -1 when absent.
	int find_weapon(const String &name) const;

	// Total loadout weight over the indexed weapons: per entry weaponweight +
	// (count <= 0 ? maxclips : count) * clipweight — the libs/def port of the
	// parent-slot terms [orig: calculate_loadout_weight @ 0x55f1f0]. Invalid
	// indices contribute nothing; a short counts array reads as -1 (default).
	double loadout_weight(const PackedInt32Array &weapon_indices,
			const PackedInt32Array &ammo_counts) const;
	// The encumbrance band for a weight (ENCUMBRANCE_*)
	// [orig: update_player_info_weight_and_weapon_icons @ 0x55f480].
	int encumbrance_class(double weight) const;
};

} // namespace godot
