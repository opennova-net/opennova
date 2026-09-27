#include "simulation/present_event_records.h"

#include "simulation/destruction_presenter.h"
#include "util/axes.h"
#include "util/color_convert.h"
#include "util/record_bind.h"
#include "util/string_convert.h"

#include <runtime/hud/feed_format.h> // chat_channel_color / chat_channel_sink
#include <runtime/world/destruction.h> // death_piece_trail_effect

#include <cstdio>
#include <cmath>
#include <cstring>

using namespace godot;
using opennova::to_gd;

// --- ThrowableVisualRow ------------------------------------------------------

Ref<ThrowableVisualRow> ThrowableVisualRow::make(int64_t p_key, int p_item_id, const Vector3 &p_pos,
		const Vector3 &p_rotation_deg, const String &p_move_effect, bool p_move_effect_live) {
	opennova::world::ThrowableVisualRow v;
	v.key = p_key;
	v.item_id = p_item_id;
	v.pos = godot_to_mission<opennova::world::Vec3>(p_pos);
	v.pitch_deg = p_rotation_deg.x;
	v.yaw_deg = p_rotation_deg.y;
	v.roll_deg = p_rotation_deg.z;
	v.move_effect = opennova::to_std(p_move_effect);
	v.move_effect_live = p_move_effect_live;
	Ref<ThrowableVisualRow> out;
	out.instantiate();
	out->assign(v);
	return out;
}

Vector3 ThrowableVisualRow::get_pos() const { return mission_to_godot(value_.pos); }
Vector3 ThrowableVisualRow::get_rotation_deg() const {
	return Vector3(value_.pitch_deg, value_.yaw_deg, value_.roll_deg);
}
String ThrowableVisualRow::get_move_effect() const { return to_gd(value_.move_effect); }

void ThrowableVisualRow::_bind_methods() {
	ClassDB::bind_static_method("ThrowableVisualRow",
			D_METHOD("make", "key", "item_id", "pos", "rotation_deg", "move_effect",
					"move_effect_live"),
			&ThrowableVisualRow::make, DEFVAL(String()), DEFVAL(true));
	OPENNOVA_RECORD_READ_ONLY(ThrowableVisualRow, Variant::INT, key)
	OPENNOVA_RECORD_READ_ONLY(ThrowableVisualRow, Variant::INT, item_id)
	OPENNOVA_RECORD_READ_ONLY(ThrowableVisualRow, Variant::VECTOR3, pos)
	OPENNOVA_RECORD_READ_ONLY(ThrowableVisualRow, Variant::VECTOR3, rotation_deg)
	OPENNOVA_RECORD_READ_ONLY(ThrowableVisualRow, Variant::STRING, move_effect)
	OPENNOVA_RECORD_READ_ONLY(ThrowableVisualRow, Variant::BOOL, move_effect_live)
}

// --- VehicleTrailVisualRow --------------------------------------------------

Ref<VehicleTrailVisualRow> VehicleTrailVisualRow::make(int p_handle, int64_t p_generation,
		int p_point, const String &p_effect, const Vector3 &p_pos, const Vector3 &p_dir,
		float p_magnitude, int p_tick) {
	opennova::world::VehicleTrailVisualRow v;
	v.handle_packed = p_handle;
	v.registry_spawn_id = uint64_t(p_generation);
	v.point = uint8_t(p_point);
	v.effect = opennova::to_std(p_effect);
	v.pos = { p_pos.x, -p_pos.z, p_pos.y };
	v.dir = { p_dir.x, -p_dir.z, p_dir.y };
	v.magnitude_q16 = uint32_t(std::llround(double(p_magnitude) * 65536.0));
	v.source_tick = uint32_t(p_tick);
	Ref<VehicleTrailVisualRow> out;
	out.instantiate();
	out->assign(v);
	return out;
}
Vector3 VehicleTrailVisualRow::get_pos() const {
	return mission_to_godot(value_.pos);
}
Vector3 VehicleTrailVisualRow::get_dir() const {
	return mission_to_godot(value_.dir);
}
String VehicleTrailVisualRow::get_effect() const {
	return to_gd(value_.effect);
}
float VehicleTrailVisualRow::get_magnitude() const {
	return float(value_.magnitude_q16) / 65536.0f;
}
void VehicleTrailVisualRow::_bind_methods() {
	ClassDB::bind_static_method("VehicleTrailVisualRow",
			D_METHOD("make", "handle", "generation", "point", "effect", "pos", "dir", "magnitude",
					"tick"),
			&VehicleTrailVisualRow::make, DEFVAL(0));
	OPENNOVA_RECORD_READ_ONLY(VehicleTrailVisualRow, Variant::INT, handle_packed)
	OPENNOVA_RECORD_READ_ONLY(VehicleTrailVisualRow, Variant::INT, registry_spawn_id)
	OPENNOVA_RECORD_READ_ONLY(VehicleTrailVisualRow, Variant::INT, point)
	OPENNOVA_RECORD_READ_ONLY(VehicleTrailVisualRow, Variant::INT, source_tick)
	OPENNOVA_RECORD_READ_ONLY(VehicleTrailVisualRow, Variant::VECTOR3, pos)
	OPENNOVA_RECORD_READ_ONLY(VehicleTrailVisualRow, Variant::VECTOR3, dir)
	OPENNOVA_RECORD_READ_ONLY(VehicleTrailVisualRow, Variant::STRING, effect)
	OPENNOVA_RECORD_READ_ONLY(VehicleTrailVisualRow, Variant::FLOAT, magnitude)
}

// --- FirePresentationEvent --------------------------------------------------

Ref<FirePresentationEvent> FirePresentationEvent::make(const Vector3 &p_origin, int p_source_bms_id,
		int p_shooter_handle, bool p_is_local_player, int p_mf_light, const Vector3 &p_forward,
		bool p_adm_arm, int p_adm_index, const String &p_effect, const String &p_action_effect,
		const String &p_action_userpoint, int p_ammo_index) {
	opennova::world::FirePresentationRow v;
	v.origin = godot_to_mission<opennova::world::Vec3>(p_origin);
	v.adm_arm = p_adm_arm;
	v.adm_index = p_adm_index;
	v.forward = godot_to_mission<opennova::world::Vec3>(p_forward);
	v.shooter_handle = p_shooter_handle;
	v.source_bms_id = p_source_bms_id;
	v.is_local_player = p_is_local_player;
	v.ammo_index = p_ammo_index;
	v.effect = opennova::to_std(p_effect);
	v.mf_light = p_mf_light;
	v.action_effect = opennova::to_std(p_action_effect);
	v.action_userpoint = opennova::to_std(p_action_userpoint);
	Ref<FirePresentationEvent> out;
	out.instantiate();
	out->assign(v);
	return out;
}

Vector3 FirePresentationEvent::get_origin() const { return mission_to_godot(value_.origin); }
Vector3 FirePresentationEvent::get_forward() const { return mission_to_godot(value_.forward); }
String FirePresentationEvent::get_effect() const { return to_gd(value_.effect); }
String FirePresentationEvent::get_action_effect() const { return to_gd(value_.action_effect); }
String FirePresentationEvent::get_action_userpoint() const { return to_gd(value_.action_userpoint); }

void FirePresentationEvent::_bind_methods() {
	ClassDB::bind_static_method("FirePresentationEvent",
			D_METHOD("make", "origin", "source_bms_id", "shooter_handle", "is_local_player",
					"mf_light", "forward", "adm_arm", "adm_index", "effect", "action_effect",
					"action_userpoint", "ammo_index"),
			&FirePresentationEvent::make, DEFVAL(0), DEFVAL(-1), DEFVAL(false), DEFVAL(0),
			DEFVAL(Vector3(0, 0, -1)), DEFVAL(false), DEFVAL(0), DEFVAL(String()),
			DEFVAL(String()), DEFVAL(String()), DEFVAL(0));
	OPENNOVA_RECORD_READ_ONLY(FirePresentationEvent, Variant::VECTOR3, origin)
	OPENNOVA_RECORD_READ_ONLY(FirePresentationEvent, Variant::BOOL, adm_arm)
	OPENNOVA_RECORD_READ_ONLY(FirePresentationEvent, Variant::INT, adm_index)
	OPENNOVA_RECORD_READ_ONLY(FirePresentationEvent, Variant::VECTOR3, forward)
	OPENNOVA_RECORD_READ_ONLY(FirePresentationEvent, Variant::INT, shooter_handle)
	OPENNOVA_RECORD_READ_ONLY(FirePresentationEvent, Variant::INT, source_bms_id)
	OPENNOVA_RECORD_READ_ONLY(FirePresentationEvent, Variant::BOOL, is_local_player)
	OPENNOVA_RECORD_READ_ONLY(FirePresentationEvent, Variant::INT, ammo_index)
	OPENNOVA_RECORD_READ_ONLY(FirePresentationEvent, Variant::STRING, effect)
	OPENNOVA_RECORD_READ_ONLY(FirePresentationEvent, Variant::INT, mf_light)
	OPENNOVA_RECORD_READ_ONLY(FirePresentationEvent, Variant::STRING, action_effect)
	OPENNOVA_RECORD_READ_ONLY(FirePresentationEvent, Variant::STRING, action_userpoint)
}

// --- FireSoundRow -----------------------------------------------------------

Ref<FireSoundRow> FireSoundRow::make(const String &p_soundset, const Vector3 &p_pos,
		int p_source_bms_id) {
	opennova::world::ReadyFireSound v;
	v.set_name = opennova::to_std(p_soundset);
	v.pos = godot_to_mission<opennova::world::Vec3>(p_pos);
	v.source_bms_id = p_source_bms_id;
	Ref<FireSoundRow> out;
	out.instantiate();
	out->assign(v);
	return out;
}

String FireSoundRow::get_soundset() const { return to_gd(value_.set_name); }
Vector3 FireSoundRow::get_pos() const { return mission_to_godot(value_.pos); }

void FireSoundRow::_bind_methods() {
	ClassDB::bind_static_method("FireSoundRow", D_METHOD("make", "soundset", "pos", "source_bms_id"),
			&FireSoundRow::make, DEFVAL(0));
	OPENNOVA_RECORD_READ_ONLY(FireSoundRow, Variant::STRING, soundset)
	OPENNOVA_RECORD_READ_ONLY(FireSoundRow, Variant::VECTOR3, pos)
	OPENNOVA_RECORD_READ_ONLY(FireSoundRow, Variant::INT, source_bms_id)
}

// --- SlotSoundRow -----------------------------------------------------------

Ref<SlotSoundRow> SlotSoundRow::make(const String &p_soundset, const Vector3 &p_pos, int p_handle,
		int p_slot) {
	opennova::world::SoundSlotEvent v;
	v.source_handle = static_cast<uint16_t>(p_handle);
	// Godot (x, y, z) -> mission-frame 16.16 (x, -z, y).
	v.pos[0] = static_cast<int32_t>(p_pos.x * 65536.0f);
	v.pos[1] = static_cast<int32_t>(-p_pos.z * 65536.0f);
	v.pos[2] = static_cast<int32_t>(p_pos.y * 65536.0f);
	v.slot = static_cast<uint8_t>(p_slot);
	std::snprintf(v.set_name, sizeof(v.set_name), "%s", p_soundset.utf8().get_data());
	Ref<SlotSoundRow> out;
	out.instantiate();
	out->assign(v);
	return out;
}

String SlotSoundRow::get_soundset() const { return String(value_.set_name); }
Vector3 SlotSoundRow::get_pos() const {
	// Mission-frame 16.16 -> godot (x, z, -y), the fire drain's mapping.
	return Vector3(static_cast<float>(value_.pos[0]) / 65536.0f,
			static_cast<float>(value_.pos[2]) / 65536.0f,
			static_cast<float>(-value_.pos[1]) / 65536.0f);
}

void SlotSoundRow::_bind_methods() {
	ClassDB::bind_static_method("SlotSoundRow", D_METHOD("make", "soundset", "pos", "handle", "slot"),
			&SlotSoundRow::make);
	OPENNOVA_RECORD_READ_ONLY(SlotSoundRow, Variant::STRING, soundset)
	OPENNOVA_RECORD_READ_ONLY(SlotSoundRow, Variant::VECTOR3, pos)
	OPENNOVA_RECORD_READ_ONLY(SlotSoundRow, Variant::INT, handle)
	OPENNOVA_RECORD_READ_ONLY(SlotSoundRow, Variant::INT, slot)
}

// --- SoundEmitterRow --------------------------------------------------------

Ref<SoundEmitterRow> SoundEmitterRow::make(int64_t p_source_spawn_id, int p_handle,
		int p_source_bms_id, const Vector3 &p_pos, int p_lane, int p_slot, int p_lifetime,
		int64_t p_emitted_tick, int p_pitch_q16, int p_volume_q8_8, bool p_source_only,
		const String &p_soundset) {
	opennova::world::SoundEmitterEvent v;
	v.source_spawn_id = static_cast<uint64_t>(p_source_spawn_id);
	v.source_handle = static_cast<uint16_t>(p_handle);
	v.pos = godot_to_mission<opennova::world::Vec3>(p_pos);
	v.source_bms_id = p_source_bms_id;
	v.emitted_tick = static_cast<uint32_t>(p_emitted_tick);
	v.lane = static_cast<uint8_t>(p_lane);
	v.slot = static_cast<uint8_t>(p_slot);
	v.lifetime_ticks = p_lifetime;
	v.pitch_q16 = p_pitch_q16;
	v.volume_q8_8 = static_cast<uint16_t>(p_volume_q8_8);
	v.source_only = p_source_only;
	v.set_name = opennova::to_std(p_soundset);
	Ref<SoundEmitterRow> out;
	out.instantiate();
	out->assign(v);
	return out;
}

Vector3 SoundEmitterRow::get_pos() const { return mission_to_godot(value_.pos); }
String SoundEmitterRow::get_soundset() const { return to_gd(value_.set_name); }

void SoundEmitterRow::_bind_methods() {
	ClassDB::bind_static_method("SoundEmitterRow",
			D_METHOD("make", "source_spawn_id", "handle", "source_bms_id", "pos", "lane", "slot",
					"lifetime", "emitted_tick", "pitch_q16", "volume_q8_8", "source_only",
					"soundset"),
			&SoundEmitterRow::make);
	OPENNOVA_RECORD_READ_ONLY(SoundEmitterRow, Variant::INT, source_spawn_id)
	OPENNOVA_RECORD_READ_ONLY(SoundEmitterRow, Variant::INT, handle)
	OPENNOVA_RECORD_READ_ONLY(SoundEmitterRow, Variant::INT, source_bms_id)
	OPENNOVA_RECORD_READ_ONLY(SoundEmitterRow, Variant::VECTOR3, pos)
	OPENNOVA_RECORD_READ_ONLY(SoundEmitterRow, Variant::INT, lane)
	OPENNOVA_RECORD_READ_ONLY(SoundEmitterRow, Variant::INT, slot)
	OPENNOVA_RECORD_READ_ONLY(SoundEmitterRow, Variant::INT, lifetime)
	OPENNOVA_RECORD_READ_ONLY(SoundEmitterRow, Variant::INT, emitted_tick)
	OPENNOVA_RECORD_READ_ONLY(SoundEmitterRow, Variant::INT, pitch_q16)
	OPENNOVA_RECORD_READ_ONLY(SoundEmitterRow, Variant::INT, volume_q8_8)
	OPENNOVA_RECORD_READ_ONLY(SoundEmitterRow, Variant::BOOL, source_only)
	OPENNOVA_RECORD_READ_ONLY(SoundEmitterRow, Variant::STRING, soundset)
}

// --- RoundImpactRow ---------------------------------------------------------

Vector3 RoundImpactRow::get_position() const { return mission_to_godot(value_.position); }
Vector3 RoundImpactRow::get_direction() const { return mission_to_godot(value_.direction); }
String RoundImpactRow::get_effect() const { return to_gd(value_.effect); }
String RoundImpactRow::get_sound() const { return to_gd(value_.sound); }
Color RoundImpactRow::get_light_color() const {
	return opennova::color_from_rgb24(value_.light_color_rgb24);
}

void RoundImpactRow::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY(RoundImpactRow, Variant::VECTOR3, position)
	OPENNOVA_RECORD_READ_ONLY(RoundImpactRow, Variant::VECTOR3, direction)
	OPENNOVA_RECORD_READ_ONLY(RoundImpactRow, Variant::STRING, effect)
	OPENNOVA_RECORD_READ_ONLY(RoundImpactRow, Variant::STRING, sound)
	OPENNOVA_RECORD_READ_ONLY(RoundImpactRow, Variant::INT, age_ticks)
	OPENNOVA_RECORD_READ_ONLY(RoundImpactRow, Variant::INT, source_tick)
	OPENNOVA_RECORD_READ_ONLY(RoundImpactRow, Variant::INT, source_order)
	OPENNOVA_RECORD_READ_ONLY(RoundImpactRow, Variant::BOOL, has_light)
	OPENNOVA_RECORD_READ_ONLY(RoundImpactRow, Variant::FLOAT, light_radius)
	OPENNOVA_RECORD_READ_ONLY(RoundImpactRow, Variant::COLOR, light_color)
	OPENNOVA_RECORD_READ_ONLY(RoundImpactRow, Variant::INT, light_ticks)
}

// --- ChatLineRow ------------------------------------------------------------

String ChatLineRow::get_text() const { return to_gd(value_.text); }
int64_t ChatLineRow::get_argb() const {
	return static_cast<int64_t>(opennova::hud::chat_channel_color(value_.channel));
}
int ChatLineRow::get_sink() const {
	return static_cast<int>(opennova::hud::chat_channel_sink(value_.channel));
}

void ChatLineRow::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY(ChatLineRow, Variant::STRING, text)
	OPENNOVA_RECORD_READ_ONLY(ChatLineRow, Variant::INT, argb)
	OPENNOVA_RECORD_READ_ONLY(ChatLineRow, Variant::INT, sink)
	OPENNOVA_RECORD_READ_ONLY(ChatLineRow, Variant::INT, channel)
}

// --- DeathPieceRow ----------------------------------------------------------

Ref<DeathPieceRow> DeathPieceRow::make(int p_slot, int64_t p_generation, int p_type_index,
		const Vector3 &p_pos, bool p_settled, int p_item_id, int p_section, float p_scale,
		float p_heading, float p_pitch, float p_roll) {
	opennova::world::DeathPieceRow v;
	v.slot = p_slot;
	v.generation = static_cast<uint64_t>(p_generation);
	v.item_id = p_item_id;
	v.section = p_section;
	v.type_index = p_type_index;
	v.scale = p_scale;
	v.pos = godot_to_mission<opennova::world::Vec3>(p_pos);
	v.heading = p_heading;
	v.pitch = p_pitch;
	v.roll = p_roll;
	v.settled = p_settled;
	Ref<DeathPieceRow> out;
	out.instantiate();
	out->assign(v);
	return out;
}

String DeathPieceRow::get_trail() const {
	// The debris-type trail effect from the ONE native table (the sim's
	// fill_death_pieces carries the witness); "" = no trail authored.
	return String(opennova::world::death_piece_trail_effect(
			static_cast<uint8_t>(value_.type_index)));
}
Vector3 DeathPieceRow::get_pos() const { return mission_to_godot(value_.pos); }

void DeathPieceRow::_bind_methods() {
	ClassDB::bind_static_method("DeathPieceRow",
			D_METHOD("make", "slot", "generation", "type_index", "pos", "settled", "item_id",
					"section", "scale", "heading", "pitch", "roll"),
			&DeathPieceRow::make, DEFVAL(false), DEFVAL(0), DEFVAL(0), DEFVAL(1.0f), DEFVAL(0.0f),
			DEFVAL(0.0f), DEFVAL(0.0f));
	OPENNOVA_RECORD_READ_ONLY(DeathPieceRow, Variant::INT, slot)
	OPENNOVA_RECORD_READ_ONLY(DeathPieceRow, Variant::INT, generation)
	OPENNOVA_RECORD_READ_ONLY(DeathPieceRow, Variant::INT, item_id)
	OPENNOVA_RECORD_READ_ONLY(DeathPieceRow, Variant::INT, section)
	OPENNOVA_RECORD_READ_ONLY(DeathPieceRow, Variant::INT, type_index)
	OPENNOVA_RECORD_READ_ONLY(DeathPieceRow, Variant::STRING, trail)
	OPENNOVA_RECORD_READ_ONLY(DeathPieceRow, Variant::FLOAT, scale)
	OPENNOVA_RECORD_READ_ONLY(DeathPieceRow, Variant::VECTOR3, pos)
	OPENNOVA_RECORD_READ_ONLY(DeathPieceRow, Variant::FLOAT, heading)
	OPENNOVA_RECORD_READ_ONLY(DeathPieceRow, Variant::FLOAT, pitch)
	OPENNOVA_RECORD_READ_ONLY(DeathPieceRow, Variant::FLOAT, roll)
	OPENNOVA_RECORD_READ_ONLY(DeathPieceRow, Variant::BOOL, settled)
}

// --- DeathPieceDraw ---------------------------------------------------------

Ref<DeathPieceDraw> DeathPieceDraw::make(int p_slot, int64_t p_generation, int p_lod_level,
		int64_t p_hidden_mask, int p_section, const Vector3 &p_pivot, const Vector3 &p_pos,
		float p_heading, float p_pitch, float p_roll, float p_scale) {
	opennova::world::DeathPieceDraw v;
	v.slot = p_slot;
	v.generation = static_cast<uint64_t>(p_generation);
	v.lod_level = p_lod_level;
	v.hidden_mask = static_cast<uint32_t>(p_hidden_mask);
	v.section = p_section;
	v.pivoted = v.hidden_mask != 0;
	v.pivot_q16 = {opennova::world::to_fixed(p_pivot.x), opennova::world::to_fixed(p_pivot.y),
			opennova::world::to_fixed(p_pivot.z)};
	v.pos = godot_to_mission<opennova::world::Vec3>(p_pos);
	v.heading = p_heading;
	v.pitch = p_pitch;
	v.roll = p_roll;
	v.scale = p_scale;
	Ref<DeathPieceDraw> out;
	out.instantiate();
	out->assign(v);
	return out;
}

Vector3 DeathPieceDraw::get_pos() const { return mission_to_godot(value_.pos); }

Transform3D DeathPieceDraw::get_transform() const {
	return DestructionPresenter::piece_draw_transform(value_);
}

void DeathPieceDraw::_bind_methods() {
	ClassDB::bind_static_method("DeathPieceDraw",
			D_METHOD("make", "slot", "generation", "lod_level", "hidden_mask", "section", "pivot",
					"pos", "heading", "pitch", "roll", "scale"),
			&DeathPieceDraw::make, DEFVAL(0.0f), DEFVAL(0.0f), DEFVAL(0.0f), DEFVAL(1.0f));
	OPENNOVA_RECORD_READ_ONLY(DeathPieceDraw, Variant::INT, slot)
	OPENNOVA_RECORD_READ_ONLY(DeathPieceDraw, Variant::INT, generation)
	OPENNOVA_RECORD_READ_ONLY(DeathPieceDraw, Variant::INT, lod_level)
	OPENNOVA_RECORD_READ_ONLY(DeathPieceDraw, Variant::INT, hidden_mask)
	OPENNOVA_RECORD_READ_ONLY(DeathPieceDraw, Variant::INT, section)
	OPENNOVA_RECORD_READ_ONLY(DeathPieceDraw, Variant::VECTOR3, pos)
	OPENNOVA_RECORD_READ_ONLY(DeathPieceDraw, Variant::TRANSFORM3D, transform)
}

// --- RoundGlowRow -----------------------------------------------------------

Ref<RoundGlowRow> RoundGlowRow::make(int64_t p_id, const Vector3 &p_pos, float p_radius,
		const Color &p_color) {
	opennova::world::RoundGlowRow v;
	v.id = static_cast<uint64_t>(p_id);
	v.pos = godot_to_mission<opennova::world::Vec3>(p_pos);
	v.radius = p_radius;
	v.color_rgb24 = opennova::argb_from_color_opaque(p_color) & 0x00FFFFFFu;
	Ref<RoundGlowRow> out;
	out.instantiate();
	out->assign(v);
	return out;
}

Vector3 RoundGlowRow::get_pos() const { return mission_to_godot(value_.pos); }
Color RoundGlowRow::get_color() const { return opennova::color_from_rgb24(value_.color_rgb24); }

void RoundGlowRow::_bind_methods() {
	ClassDB::bind_static_method("RoundGlowRow", D_METHOD("make", "id", "pos", "radius", "color"),
			&RoundGlowRow::make);
	OPENNOVA_RECORD_READ_ONLY(RoundGlowRow, Variant::INT, id)
	OPENNOVA_RECORD_READ_ONLY(RoundGlowRow, Variant::VECTOR3, pos)
	OPENNOVA_RECORD_READ_ONLY(RoundGlowRow, Variant::FLOAT, radius)
	OPENNOVA_RECORD_READ_ONLY(RoundGlowRow, Variant::COLOR, color)
}

// --- MissionEffect ----------------------------------------------------------

Ref<MissionEffect> MissionEffect::make(const String &p_kind, int p_a, int p_b, int p_c,
		const String &p_text, int p_d, int p_wire_handle) {
	opennova::world::Effect v;
	v.kind = opennova::to_std(p_kind);
	v.a = p_a;
	v.b = p_b;
	v.c = p_c;
	v.d = p_d;
	v.str = opennova::to_std(p_text);
	Ref<MissionEffect> out;
	out.instantiate();
	out->assign(v, p_wire_handle);
	return out;
}

String MissionEffect::get_kind() const { return to_gd(value_.kind); }
String MissionEffect::get_text() const { return to_gd(value_.str); }

void MissionEffect::_bind_methods() {
	ClassDB::bind_static_method("MissionEffect",
			D_METHOD("make", "kind", "a", "b", "c", "text", "d", "wire_handle"),
			&MissionEffect::make, DEFVAL(0), DEFVAL(0), DEFVAL(0), DEFVAL(String()), DEFVAL(0),
			DEFVAL(-1));
	OPENNOVA_RECORD_READ_ONLY(MissionEffect, Variant::STRING, kind)
	OPENNOVA_RECORD_READ_ONLY(MissionEffect, Variant::INT, a)
	OPENNOVA_RECORD_READ_ONLY(MissionEffect, Variant::INT, b)
	OPENNOVA_RECORD_READ_ONLY(MissionEffect, Variant::INT, c)
	OPENNOVA_RECORD_READ_ONLY(MissionEffect, Variant::INT, d)
	OPENNOVA_RECORD_READ_ONLY(MissionEffect, Variant::INT, wire_handle)
	OPENNOVA_RECORD_READ_ONLY(MissionEffect, Variant::STRING, text)
}
