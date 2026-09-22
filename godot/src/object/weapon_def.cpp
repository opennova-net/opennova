#include "object/weapon_def.h"

#include "util/string_convert.h"

#include <runtime/hud/sight_overlay.h>
#include <base/io/fixed.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

using namespace opennova::def;

namespace godot {

namespace {

template <size_t N>
void set_text(char (&p_field)[N], const String &p_value) {
	std::snprintf(p_field, N, "%s", opennova::to_std(p_value).c_str());
}

PackedFloat32Array six(const float values[6]) {
	PackedFloat32Array out;
	for (int k = 0; k < 6; ++k) out.push_back(values[k]);
	return out;
}

void set_six(float values[6], const PackedFloat32Array &from) {
	for (int k = 0; k < 6; ++k) values[k] = k < from.size() ? from[k] : 0.0f;
}

PackedFloat32Array pose_columns(const float position[3], const int32_t rotation[3]) {
	PackedFloat32Array out;
	for (int k = 0; k < 3; ++k) out.push_back(position[k]);
	for (int k = 0; k < 3; ++k)
		out.push_back(static_cast<float>(rotation[k] / opennova::io::kFp16OneD));
	return out;
}

void set_pose_columns(float position[3], int32_t rotation[3], const PackedFloat32Array &from) {
	for (int k = 0; k < 3; ++k) position[k] = k < from.size() ? from[k] : 0.0f;
	for (int k = 0; k < 3; ++k)
		rotation[k] = k + 3 < from.size()
				? opennova::io::float_to_fp16_16_sat(from[k + 3]) : 0;
}

} // namespace

// --- WeaponSightRow -----------------------------------------------------------

String WeaponSightRow::get_texture() const {
	return String(value_.texture);
}

Ref<WeaponSightRow> WeaponSightRow::make(const String &p_texture, int p_x1, int p_y1, int p_x2,
		int p_y2, int p_blend, bool p_scale, bool p_slide, int p_slide_frames) {
	Ref<WeaponSightRow> row;
	row.instantiate();
	set_text(row->value_.texture, p_texture);
	row->value_.x1 = p_x1;
	row->value_.y1 = p_y1;
	row->value_.x2 = p_x2;
	row->value_.y2 = p_y2;
	row->value_.blend = p_blend;
	row->value_.scale = p_scale ? 1 : 0;
	row->value_.slide = p_slide ? 1 : 0;
	row->value_.slide_frames = p_slide_frames;
	return row;
}

Rect2 WeaponSightRow::evaluate_rect(int p_sight_scale_index, int p_slide_multiplier) const {
	opennova::hud::SightRowSpec spec;
	spec.x1 = value_.x1;
	spec.y1 = value_.y1;
	spec.x2 = value_.x2;
	spec.y2 = value_.y2;
	spec.scale = value_.scale != 0;
	spec.slide = value_.slide != 0;
	spec.slide_frames = value_.slide_frames;
	const opennova::hud::SightRect rect = opennova::hud::sight_row_rect(spec,
			p_sight_scale_index, static_cast<int32_t>(p_slide_multiplier));
	return Rect2(static_cast<real_t>(rect.x1), static_cast<real_t>(rect.y1),
			static_cast<real_t>(rect.x2 - rect.x1), static_cast<real_t>(rect.y2 - rect.y1));
}

void WeaponSightRow::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_texture"), &WeaponSightRow::get_texture);
	ClassDB::bind_method(D_METHOD("get_x1"), &WeaponSightRow::get_x1);
	ClassDB::bind_method(D_METHOD("get_y1"), &WeaponSightRow::get_y1);
	ClassDB::bind_method(D_METHOD("get_x2"), &WeaponSightRow::get_x2);
	ClassDB::bind_method(D_METHOD("get_y2"), &WeaponSightRow::get_y2);
	ClassDB::bind_method(D_METHOD("get_blend"), &WeaponSightRow::get_blend);
	ClassDB::bind_method(D_METHOD("is_scale"), &WeaponSightRow::is_scale);
	ClassDB::bind_method(D_METHOD("evaluate_rect", "sight_scale_index", "slide_multiplier"),
			&WeaponSightRow::evaluate_rect);
	ClassDB::bind_static_method("WeaponSightRow",
			D_METHOD("make", "texture", "x1", "y1", "x2", "y2", "blend", "scale", "slide",
					"slide_frames"),
			&WeaponSightRow::make, DEFVAL(false), DEFVAL(false), DEFVAL(0));
}

// --- WeaponActionRow ------------------------------------------------------------

void WeaponActionRow::assign(const DefWeaponAction &p_value) {
	value_ = p_value;
	// The writer-only raw line buffer stays with the parse.
	value_.raw_lines = nullptr;
	value_.raw_lines_count = 0;
}

Ref<WeaponActionRow> WeaponActionRow::make(const String &p_name, int p_delaystart,
		int p_delayend, const String &p_anim, const String &p_soundset,
		const String &p_soundsetend, const String &p_function, const String &p_particle,
		const String &p_particleuserpoint) {
	Ref<WeaponActionRow> row;
	row.instantiate();
	row->set_name(p_name);
	row->set_delaystart(p_delaystart);
	row->set_delayend(p_delayend);
	row->set_anim(p_anim);
	row->set_soundset(p_soundset);
	row->set_soundsetend(p_soundsetend);
	row->set_function(p_function);
	row->set_particle(p_particle);
	row->set_particleuserpoint(p_particleuserpoint);
	return row;
}

#define WEAPON_ACTION_TEXT(m_name)                                                          \
	String WeaponActionRow::get_##m_name() const { return String(value_.m_name); }           \
	void WeaponActionRow::set_##m_name(const String &p_value) { set_text(value_.m_name, p_value); }
WEAPON_ACTION_TEXT(name)
WEAPON_ACTION_TEXT(anim)
WEAPON_ACTION_TEXT(function)
WEAPON_ACTION_TEXT(soundset)
WEAPON_ACTION_TEXT(soundsetend)
WEAPON_ACTION_TEXT(particle)
WEAPON_ACTION_TEXT(particleuserpoint)
#undef WEAPON_ACTION_TEXT

void WeaponActionRow::_bind_methods() {
	ClassDB::bind_static_method("WeaponActionRow",
			D_METHOD("make", "name", "delaystart", "delayend", "anim", "soundset", "soundsetend",
					"function", "particle", "particleuserpoint"),
			&WeaponActionRow::make, DEFVAL(String()), DEFVAL(String()), DEFVAL(String()),
			DEFVAL(String()), DEFVAL(String()), DEFVAL(String()));
#define WEAPON_ACTION_PROP(m_type, m_name)                                                   \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &WeaponActionRow::set_##m_name); \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &WeaponActionRow::get_##m_name);        \
	ADD_PROPERTY(PropertyInfo(m_type, #m_name), "set_" #m_name, "get_" #m_name)
	WEAPON_ACTION_PROP(Variant::STRING, name);
	WEAPON_ACTION_PROP(Variant::STRING, anim);
	WEAPON_ACTION_PROP(Variant::STRING, function);
	WEAPON_ACTION_PROP(Variant::INT, action_value);
	WEAPON_ACTION_PROP(Variant::INT, delaystart);
	WEAPON_ACTION_PROP(Variant::INT, delayend);
	WEAPON_ACTION_PROP(Variant::STRING, soundset);
	WEAPON_ACTION_PROP(Variant::STRING, soundsetend);
	WEAPON_ACTION_PROP(Variant::STRING, particle);
	WEAPON_ACTION_PROP(Variant::STRING, particleuserpoint);
#undef WEAPON_ACTION_PROP
}

// --- WeaponDef -------------------------------------------------------------------

void WeaponDef::assign(int p_index, const DefWeaponDef &p_value) {
	index_ = p_index;
	value_ = p_value;
	sights_.assign(p_value.sights, p_value.sights + p_value.sights_count);
	actions_.assign(p_value.actions, p_value.actions + p_value.actions_count);
	// The writer-only raw line buffers stay with the parse.
	value_.raw_lines = nullptr;
	value_.raw_lines_count = 0;
	for (DefWeaponAction &action : actions_) {
		action.raw_lines = nullptr;
		action.raw_lines_count = 0;
	}
	rebind_rows();
}

void WeaponDef::rebind_rows() {
	value_.sights = sights_.empty() ? nullptr : sights_.data();
	value_.sights_count = sights_.size();
	value_.actions = actions_.empty() ? nullptr : actions_.data();
	value_.actions_count = actions_.size();
}

Ref<WeaponDef> WeaponDef::copy() const {
	Ref<WeaponDef> out;
	out.instantiate();
	out->assign(index_, value_);
	return out;
}

#define WEAPON_DEF_TEXT(m_name, m_field)                                                     \
	String WeaponDef::get_##m_name() const { return String(value_.m_field); }                \
	void WeaponDef::set_##m_name(const String &p_value) { set_text(value_.m_field, p_value); }
WEAPON_DEF_TEXT(name, weapon_name)
WEAPON_DEF_TEXT(display_textid, loadout_menu_textid)
WEAPON_DEF_TEXT(round_type, round_type)
WEAPON_DEF_TEXT(icon, loadout_menu_icon)
WEAPON_DEF_TEXT(animadm, animadm)
WEAPON_DEF_TEXT(gfx1, gfx1)
WEAPON_DEF_TEXT(gfx3, gfx3)
WEAPON_DEF_TEXT(hudclipgfx_texture, hudclipgfx_texture)
WEAPON_DEF_TEXT(hudrndgfx_texture, hudrndgfx_texture)
#undef WEAPON_DEF_TEXT

PackedFloat32Array WeaponDef::get_pos() const { return pose_columns(value_.pos, value_.pos_rotation_deg_q16); }
void WeaponDef::set_pos(const PackedFloat32Array &p_value) { set_pose_columns(value_.pos, value_.pos_rotation_deg_q16, p_value); }
PackedFloat32Array WeaponDef::get_tpos() const { return pose_columns(value_.tpos, value_.tpos_rotation_deg_q16); }
void WeaponDef::set_tpos(const PackedFloat32Array &p_value) { set_pose_columns(value_.tpos, value_.tpos_rotation_deg_q16, p_value); }
PackedFloat32Array WeaponDef::get_error() const { return six(value_.error); }
void WeaponDef::set_error(const PackedFloat32Array &p_value) { set_six(value_.error, p_value); }

Vector3 WeaponDef::get_pos_units() const {
	return Vector3(value_.pos[0], value_.pos[1], value_.pos[2]);
}

Vector3 WeaponDef::get_rot_bias_deg() const {
	return Vector3(value_.pos_rotation_deg_q16[0] / opennova::io::kFp16OneD,
			value_.pos_rotation_deg_q16[1] / opennova::io::kFp16OneD,
			value_.pos_rotation_deg_q16[2] / opennova::io::kFp16OneD);
}

Vector3 WeaponDef::get_tpos_units() const {
	return Vector3(value_.tpos[0], value_.tpos[1], value_.tpos[2]);
}

int WeaponDef::get_sight_slide_multiplier() const {
	opennova::hud::ScopeZeroInputs in;
	in.slot_zero_word = 0;
	in.scope_max_zero_steps = value_.scope_max_zero_steps;
	in.scope_zero_step = value_.scope_zero_step;
	in.scope_zero_default = value_.scope_zero_default;
	in.rangefinder_q16 = 0;
	in.scoring_disabled = false;
	return static_cast<int>(opennova::hud::sight_slide_multiplier(in));
}

Vector2i WeaponDef::get_hudclipgfx_offset() const {
	return Vector2i(value_.hudclipgfx_offset[0], value_.hudclipgfx_offset[1]);
}

void WeaponDef::set_hudclipgfx_offset(const Vector2i &p_value) {
	value_.hudclipgfx_offset[0] = p_value.x;
	value_.hudclipgfx_offset[1] = p_value.y;
}

Vector2i WeaponDef::get_hudrndgfx_offset() const {
	return Vector2i(value_.hudrndgfx_offset[0], value_.hudrndgfx_offset[1]);
}

void WeaponDef::set_hudrndgfx_offset(const Vector2i &p_value) {
	value_.hudrndgfx_offset[0] = p_value.x;
	value_.hudrndgfx_offset[1] = p_value.y;
}

Vector3i WeaponDef::get_hudrndgfx_layout() const {
	return Vector3i(value_.hudrndgfx_layout[0], value_.hudrndgfx_layout[1],
			value_.hudrndgfx_layout[2]);
}

void WeaponDef::set_hudrndgfx_layout(const Vector3i &p_value) {
	value_.hudrndgfx_layout[0] = p_value.x;
	value_.hudrndgfx_layout[1] = p_value.y;
	value_.hudrndgfx_layout[2] = p_value.z;
}

TypedArray<WeaponSightRow> WeaponDef::get_sights() const {
	TypedArray<WeaponSightRow> out;
	for (const DefSightEntry &entry : sights_) {
		Ref<WeaponSightRow> row;
		row.instantiate();
		row->assign(entry);
		out.push_back(row);
	}
	return out;
}

void WeaponDef::set_actions(const TypedArray<WeaponActionRow> &p_rows) {
	actions_.clear();
	actions_.reserve(static_cast<size_t>(p_rows.size()));
	for (int i = 0; i < p_rows.size(); ++i) {
		const Ref<WeaponActionRow> row = p_rows[i];
		if (row.is_valid()) actions_.push_back(row->value());
	}
	rebind_rows();
}

void WeaponDef::_bind_methods() {
	ClassDB::bind_method(D_METHOD("copy"), &WeaponDef::copy);
	ClassDB::bind_method(D_METHOD("get_sights"), &WeaponDef::get_sights);
	ClassDB::bind_method(D_METHOD("set_actions", "rows"), &WeaponDef::set_actions);
#define WEAPON_DEF_PROP(m_type, m_name)                                                     \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &WeaponDef::set_##m_name);       \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &WeaponDef::get_##m_name);              \
	ADD_PROPERTY(PropertyInfo(m_type, #m_name), "set_" #m_name, "get_" #m_name)
	WEAPON_DEF_PROP(Variant::INT, index);
	WEAPON_DEF_PROP(Variant::STRING, name);
	WEAPON_DEF_PROP(Variant::STRING, display_textid);
	WEAPON_DEF_PROP(Variant::STRING, round_type);
	WEAPON_DEF_PROP(Variant::STRING, icon);
	WEAPON_DEF_PROP(Variant::INT, loadout_subclasses);
	WEAPON_DEF_PROP(Variant::INT, slot);
	WEAPON_DEF_PROP(Variant::FLOAT, weight);
	WEAPON_DEF_PROP(Variant::FLOAT, clip_weight);
	WEAPON_DEF_PROP(Variant::INT, clipsize);
	WEAPON_DEF_PROP(Variant::INT, startrounds);
	WEAPON_DEF_PROP(Variant::INT, maxclips);
	WEAPON_DEF_PROP(Variant::INT, flags);
	WEAPON_DEF_PROP(Variant::INT, flags2);
	WEAPON_DEF_PROP(Variant::FLOAT, scope_max_mag);
	WEAPON_DEF_PROP(Variant::INT, scope_min_mag);
	WEAPON_DEF_PROP(Variant::STRING, animadm);
	WEAPON_DEF_PROP(Variant::STRING, gfx1);
	WEAPON_DEF_PROP(Variant::STRING, gfx3);
	WEAPON_DEF_PROP(Variant::PACKED_FLOAT32_ARRAY, pos);
	WEAPON_DEF_PROP(Variant::PACKED_FLOAT32_ARRAY, tpos);
	WEAPON_DEF_PROP(Variant::FLOAT, renderfov);
	WEAPON_DEF_PROP(Variant::INT, special_hold);
	WEAPON_DEF_PROP(Variant::INT, attack_anim);
	WEAPON_DEF_PROP(Variant::INT, run_anim);
	WEAPON_DEF_PROP(Variant::INT, heat_per_shot);
	WEAPON_DEF_PROP(Variant::INT, heat_decay_per_tick);
	WEAPON_DEF_PROP(Variant::INT, heat_glow_threshold);
	WEAPON_DEF_PROP(Variant::INT, scope_max_zero_steps);
	WEAPON_DEF_PROP(Variant::INT, scope_zero_step);
	WEAPON_DEF_PROP(Variant::INT, scope_zero_default);
	WEAPON_DEF_PROP(Variant::INT, scope_zero_extra);
	ClassDB::bind_method(D_METHOD("get_sight_slide_multiplier"),
			&WeaponDef::get_sight_slide_multiplier);
	WEAPON_DEF_PROP(Variant::PACKED_FLOAT32_ARRAY, error);
	WEAPON_DEF_PROP(Variant::STRING, hudclipgfx_texture);
	WEAPON_DEF_PROP(Variant::VECTOR2I, hudclipgfx_offset);
	ClassDB::bind_method(D_METHOD("get_pos_units"), &WeaponDef::get_pos_units);
	ClassDB::bind_method(D_METHOD("get_rot_bias_deg"), &WeaponDef::get_rot_bias_deg);
	ClassDB::bind_method(D_METHOD("get_tpos_units"), &WeaponDef::get_tpos_units);
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "pos_units", PROPERTY_HINT_NONE, "",
						 PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY),
			"", "get_pos_units");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "rot_bias_deg", PROPERTY_HINT_NONE, "",
						 PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY),
			"", "get_rot_bias_deg");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "tpos_units", PROPERTY_HINT_NONE, "",
						 PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY),
			"", "get_tpos_units");
	WEAPON_DEF_PROP(Variant::STRING, hudrndgfx_texture);
	WEAPON_DEF_PROP(Variant::VECTOR2I, hudrndgfx_offset);
	WEAPON_DEF_PROP(Variant::VECTOR3I, hudrndgfx_layout);
#undef WEAPON_DEF_PROP
}

// --- ArmoryClassRow -------------------------------------------------------------

void ArmoryClassRow::assign(int p_value, const char *p_text_key) {
	value_ = p_value;
	text_key_ = String(p_text_key);
}

void ArmoryClassRow::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_value"), &ArmoryClassRow::get_value);
	ClassDB::bind_method(D_METHOD("get_text_key"), &ArmoryClassRow::get_text_key);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "value", PROPERTY_HINT_NONE, "",
						 PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY),
			"", "get_value");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "text_key", PROPERTY_HINT_NONE, "",
						 PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY),
			"", "get_text_key");
}

} // namespace godot
