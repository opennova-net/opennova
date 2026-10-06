#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/vector3i.hpp>

#include <formats/def/def.h>

#include <vector>

namespace godot {

// One authored SIGHTS card row of a weapon.def entry (DefSightEntry): the
// texture and its virtual 1024x768 rect, the blend mode, the scale/slide
// flags. Draw order is the authored order (see <runtime/hud/hud_frame.h>).
// Authorable through make(): a test builds a card from these.
class WeaponSightRow : public RefCounted {
	GDCLASS(WeaponSightRow, RefCounted)

	opennova::def::DefSightEntry value_ = {};

protected:
	static void _bind_methods();

public:
	void assign(const opennova::def::DefSightEntry &p_value) { value_ = p_value; }

	String get_texture() const;
	int get_x1() const { return value_.x1; }
	int get_y1() const { return value_.y1; }
	int get_x2() const { return value_.x2; }
	int get_y2() const { return value_.y2; }
	// DefSightBlendMode transport value.
	int get_blend() const { return value_.blend; }
	bool is_scale() const { return value_.scale != 0; }
	// The row's draw rectangle in the 1024x768 design space for a sight-scale
	// index and a scope-zero slide multiplier: plain, scaled about its centre,
	// or slid (the engine's sight_row_rect, <runtime/hud/sight_overlay.h>,
	// carries the three-mode policy).
	Rect2 evaluate_rect(int p_sight_scale_index, int p_slide_multiplier) const;

	// A row from its texture, 1024x768 rect and blend mode plus the optional
	// scale/slide flags.
	static Ref<WeaponSightRow> make(const String &p_texture, int p_x1, int p_y1, int p_x2,
			int p_y2, int p_blend, bool p_scale = false, bool p_slide = false,
			int p_slide_frames = 0);
};

// One ACTION block of a weapon.def entry (DefWeaponAction): the action name,
// its .adm clip, the function, the two delays, and the per-action audio and
// effect hooks (see <runtime/world/weapon_fsm.h>). Authorable: a test builds
// a weapon's action ladder from these.
class WeaponActionRow : public RefCounted {
	GDCLASS(WeaponActionRow, RefCounted)

	opennova::def::DefWeaponAction value_ = {};

protected:
	static void _bind_methods();

public:
	void assign(const opennova::def::DefWeaponAction &p_value);
	const opennova::def::DefWeaponAction &value() const { return value_; }

	// A row from its name and delays plus the optional clip and hooks — the
	// fields a synthetic def authors, in the order they are usually given.
	static Ref<WeaponActionRow> make(const String &p_name, int p_delaystart, int p_delayend,
			const String &p_anim = String(), const String &p_soundset = String(),
			const String &p_soundsetend = String(), const String &p_function = String(),
			const String &p_particle = String(), const String &p_particleuserpoint = String());

#define WEAPON_ACTION_TEXT(m_name)      \
	String get_##m_name() const;        \
	void set_##m_name(const String &p_value);
	WEAPON_ACTION_TEXT(name)
	WEAPON_ACTION_TEXT(anim)
	WEAPON_ACTION_TEXT(function)
	WEAPON_ACTION_TEXT(soundset)
	WEAPON_ACTION_TEXT(soundsetend)
	WEAPON_ACTION_TEXT(particle)
	WEAPON_ACTION_TEXT(particleuserpoint)
#undef WEAPON_ACTION_TEXT
	int get_action_value() const { return value_.action_value; }
	void set_action_value(int p_value) { value_.action_value = p_value; }
	int get_delaystart() const { return value_.delaystart; }
	void set_delaystart(int p_value) { value_.delaystart = p_value; }
	int get_delayend() const { return value_.delayend; }
	void set_delayend(int p_value) { value_.delayend = p_value; }
};

// One weapon.def entry as a typed record: a by-value copy of the parsed
// DefWeaponDef (its SIGHTS and ACTION rows deep-copied), read by the loadout
// screens, the HUD, the viewmodel rig and the simulation's weapon install.
// Every field is read-write so a test authors a synthetic def the same way the
// parse fills a real one. Field semantics and witnesses live on the struct
// (<formats/def/def.h>).
class WeaponDef : public RefCounted {
	GDCLASS(WeaponDef, RefCounted)

	opennova::def::DefWeaponDef value_ = {};
	std::vector<opennova::def::DefSightEntry> sights_;
	std::vector<opennova::def::DefWeaponAction> actions_;
	int index_ = -1;

	void rebind_rows();

protected:
	static void _bind_methods();

public:
	// Copy the row (the parse keeps its own arrays; this record owns its copy).
	void assign(int p_index, const opennova::def::DefWeaponDef &p_value);
	// The record as the engine reads it (rows bound to this record's storage).
	const opennova::def::DefWeaponDef &value() const { return value_; }
	// An independent copy (a test derives a variant without touching the table's row).
	Ref<WeaponDef> copy() const;

	// Table index in the retained weapon.def parse (-1 for an authored record).
	int get_index() const { return index_; }
	void set_index(int p_index) { index_ = p_index; }

#define WEAPON_DEF_TEXT(m_name)         \
	String get_##m_name() const;        \
	void set_##m_name(const String &p_value);
	WEAPON_DEF_TEXT(name)             // weapon "<id>", the display fallback
	WEAPON_DEF_TEXT(display_textid)   // GameText "WepDes" key
	WEAPON_DEF_TEXT(round_type)       // ammo key (GameText "WepDes")
	WEAPON_DEF_TEXT(icon)             // loadout_menu_icon
	WEAPON_DEF_TEXT(animadm)
	WEAPON_DEF_TEXT(gfx1)
	WEAPON_DEF_TEXT(gfx3)
	WEAPON_DEF_TEXT(hudclipgfx_texture)
	WEAPON_DEF_TEXT(hudrndgfx_texture)
#undef WEAPON_DEF_TEXT

#define WEAPON_DEF_SCALAR(m_type, m_name, m_field)              \
	m_type get_##m_name() const { return value_.m_field; }      \
	void set_##m_name(m_type p_value) { value_.m_field = p_value; }
	WEAPON_DEF_SCALAR(int, loadout_subclasses, loadout_subclasses)
	WEAPON_DEF_SCALAR(int, slot, weapon_class_slot)  // WeaponDatabase.SLOT_*
	WEAPON_DEF_SCALAR(float, weight, weaponweight)
	WEAPON_DEF_SCALAR(float, clip_weight, clipweight)
	WEAPON_DEF_SCALAR(int, clipsize, clipsize)
	WEAPON_DEF_SCALAR(int, startrounds, startrounds)
	WEAPON_DEF_SCALAR(int, maxclips, maxclips)
	WEAPON_DEF_SCALAR(int, flags, flags)
	WEAPON_DEF_SCALAR(int, flags2, flags2)
	WEAPON_DEF_SCALAR(float, scope_max_mag, scope_max_mag)
	WEAPON_DEF_SCALAR(int, scope_min_mag, scope_min_mag)
	WEAPON_DEF_SCALAR(int, scope_initial_mag, scope_max_mag_arg2)
	WEAPON_DEF_SCALAR(float, renderfov, renderfov)
	WEAPON_DEF_SCALAR(int, special_hold, special_hold)
	WEAPON_DEF_SCALAR(int, attack_anim, attack_anim)
	WEAPON_DEF_SCALAR(int, run_anim, run_anim)
	WEAPON_DEF_SCALAR(int, heat_per_shot, heat_per_shot)
	WEAPON_DEF_SCALAR(int, heat_decay_per_tick, heat_decay_per_tick)
	WEAPON_DEF_SCALAR(int, heat_glow_threshold, heat_glow_threshold)
	// The scope-zero table ('scope_max_zero <maxSteps> <stepMetres>
	// <defaultMetres> [<extra>]'; the struct in <formats/def/def.h> carries the
	// witness).
	WEAPON_DEF_SCALAR(int, scope_max_zero_steps, scope_max_zero_steps)
	WEAPON_DEF_SCALAR(int, scope_zero_step, scope_zero_step)
	WEAPON_DEF_SCALAR(int, scope_zero_default, scope_zero_default)
	WEAPON_DEF_SCALAR(int, scope_zero_extra, scope_zero_extra)
#undef WEAPON_DEF_SCALAR

	// The SIGHTS card's `slide` multiplier for this def at its DEFAULT zero:
	// the engine's sight_slide_multiplier (<runtime/hud/sight_overlay.h>) over
	// the scope-zero table with the slot's zero word 0 and no rangefinder
	// sample. The manual-word and rangefinder arms wait on the scope-zero
	// adjust port (D-WPN-8) and contribute nothing here.
	int get_sight_slide_multiplier() const;

	// xyz raw file units (/256 = world), then yaw/pitch/roll degrees.
	PackedFloat32Array get_pos() const;
	void set_pos(const PackedFloat32Array &p_value);
	PackedFloat32Array get_tpos() const;  // the ADS variant
	void set_tpos(const PackedFloat32Array &p_value);
	// The 6-row dispersion table in degrees (hip prone/crouch/stand, scoped ...).
	PackedFloat32Array get_error() const;
	void set_error(const PackedFloat32Array &p_value);
	Vector2i get_hudclipgfx_offset() const;
	// The authored POS six as presentation reads it -- xyz in mission units
	// and the rotation bias in degrees -- and TPOS's xyz (the ADS variant).
	Vector3 get_pos_units() const;
	Vector3 get_rot_bias_deg() const;
	Vector3 get_tpos_units() const;
	void set_hudclipgfx_offset(const Vector2i &p_value);
	Vector2i get_hudrndgfx_offset() const;
	void set_hudrndgfx_offset(const Vector2i &p_value);
	Vector3i get_hudrndgfx_layout() const;
	void set_hudrndgfx_layout(const Vector3i &p_value);
	TypedArray<WeaponSightRow> get_sights() const;
	// The ACTION ladder the weapon FSM bakes (weapon_fsm.h).
	TypedArray<WeaponActionRow> get_actions() const;
	void set_actions(const TypedArray<WeaponActionRow> &p_rows);
};

// One armory PLAYER_CLASS spin row (world/player_loadout.h kArmoryClassCatalog).
class ArmoryClassRow : public RefCounted {
	GDCLASS(ArmoryClassRow, RefCounted)

	int value_ = 0;
	String text_key_;

protected:
	static void _bind_methods();

public:
	void assign(int p_value, const char *p_text_key);
	int get_value() const { return value_; }
	String get_text_key() const { return text_key_; }
};

} // namespace godot
