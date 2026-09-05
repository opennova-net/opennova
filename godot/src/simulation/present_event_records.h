#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <runtime/replication/client_state.h> // ClientChatLine
#include <runtime/world/fire_sound.h> // ReadyFireSound
#include <runtime/world/present_drains.h> // the present drain rows (mission space)
#include <runtime/world/sound_emitter_mailbox.h> // SoundEmitterEvent
#include <runtime/world/world.h> // SoundSlotEvent, Effect

#include <cstdint>

// The per-tick presentation records Simulation hands the present passes and
// the GUT data legs: each is a value wrapper over the engine row (ADR 0043
// d10 — `engine::X value_` plus forwarding getters, no mirrored members). The
// C++ present passes read the engine vectors directly; a record is minted
// only for a GDScript reader, and a static `make(...)` exists only where a
// test authors the row through a pass's public data leg. Every position
// crosses in Godot space (mission (x, y, z) -> (x, z, -y)); the witnesses
// live on the fills and on the engine state each drain reads.

namespace godot {

// One item-modeled throwable per frame (Simulation::get_throwable_visuals;
// world::ThrowableVisualRow carries the field notes).
class ThrowableVisualRow : public RefCounted {
	GDCLASS(ThrowableVisualRow, RefCounted)

	opennova::world::ThrowableVisualRow value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::world::ThrowableVisualRow &p_value) { value_ = p_value; }
	const opennova::world::ThrowableVisualRow &value() const { return value_; }
	// The test constructor (the throwable present pass's data leg): the
	// Godot-space position and the placer euler (pitch, mission yaw, roll).
	static Ref<ThrowableVisualRow> make(int64_t p_key, int p_item_id, const Vector3 &p_pos,
			const Vector3 &p_rotation_deg, const String &p_move_effect = String(),
			bool p_move_effect_live = true);

	int64_t get_key() const { return value_.key; }
	int get_item_id() const { return value_.item_id; }
	Vector3 get_pos() const;
	Vector3 get_rotation_deg() const;
	String get_move_effect() const;
	bool get_move_effect_live() const { return value_.move_effect_live; }
};

// One even-tick cbot wake sample (Simulation::get_vehicle_wake_visuals;
// world::VehicleWakeVisualRow carries the identity and lane notes).
class VehicleWakeVisualRow : public RefCounted {
	GDCLASS(VehicleWakeVisualRow, RefCounted)

	opennova::world::VehicleWakeVisualRow value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::world::VehicleWakeVisualRow &p_value) { value_ = p_value; }
	const opennova::world::VehicleWakeVisualRow &value() const { return value_; }
	// Test constructor: position/water height are Godot-space values; the
	// rotation is the canonical (pitch, mission yaw, roll) degree triple.
	static Ref<VehicleWakeVisualRow> make(int p_handle_packed, int64_t p_registry_spawn_id,
			const Vector3 &p_pos, const Vector3 &p_rotation_deg, float p_water_height,
			bool p_afloat = true, const String &p_w3_effect = String(),
			const String &p_w3_userpoint = String(), float p_w3_magnitude = 0.0f,
			const String &p_w4_effect = String(), const String &p_w4_userpoint = String(),
			float p_w4_magnitude = 0.0f, int p_bms_id = 0, int p_origin_kind = 255,
			int p_origin_index = 0xFFFFFF, int p_item_id = 0, int p_source_tick = 0);

	int get_handle_packed() const { return value_.handle_packed; }
	int64_t get_registry_spawn_id() const {
		return static_cast<int64_t>(value_.registry_spawn_id);
	}
	int get_item_id() const { return value_.item_id; }
	int get_bms_id() const { return value_.bms_id; }
	int64_t get_spawn_origin() const { return value_.spawn_origin; }
	int get_source_tick() const { return static_cast<int>(value_.source_tick); }
	Vector3 get_pos() const;
	Vector3 get_rotation_deg() const;
	float get_water_height() const;
	bool get_afloat() const { return value_.afloat; }
	String get_w3_effect() const;
	String get_w3_userpoint() const;
	float get_w3_magnitude() const;
	String get_w4_effect() const;
	String get_w4_userpoint() const;
	float get_w4_magnitude() const;
};

// One round spawned since the last drain (Simulation::drain_fire_presentation_events;
// world::FirePresentationRow carries the arm notes).
class FirePresentationEvent : public RefCounted {
	GDCLASS(FirePresentationEvent, RefCounted)

	opennova::world::FirePresentationRow value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::world::FirePresentationRow &p_value) { value_ = p_value; }
	const opennova::world::FirePresentationRow &value() const { return value_; }
	// The test constructor (the fire pass's data leg): the Godot-space origin
	// and forward, the shooter facts and the two arms' effect legs.
	static Ref<FirePresentationEvent> make(const Vector3 &p_origin, int p_source_bms_id = 0,
			int p_shooter_handle = -1, bool p_is_local_player = false, int p_mf_light = 0,
			const Vector3 &p_forward = Vector3(0, 0, -1), bool p_adm_arm = false,
			int p_adm_index = 0, const String &p_effect = String(),
			const String &p_action_effect = String(), const String &p_action_userpoint = String(),
			int p_ammo_index = 0);

	Vector3 get_origin() const;
	bool get_adm_arm() const { return value_.adm_arm; }
	int get_adm_index() const { return value_.adm_index; }
	Vector3 get_forward() const;
	int get_shooter_handle() const { return value_.shooter_handle; }
	int get_source_bms_id() const { return value_.source_bms_id; }
	bool get_is_local_player() const { return value_.is_local_player; }
	int get_ammo_index() const { return value_.ammo_index; }
	String get_effect() const;
	int get_mf_light() const { return value_.mf_light; }
	String get_action_effect() const;
	String get_action_userpoint() const;
};

// One ready fire one-shot on the logic clock (world::ReadyFireSound): the
// sound set name, its position and the source's bms id for the set's
// max-range cull.
class FireSoundRow : public RefCounted {
	GDCLASS(FireSoundRow, RefCounted)

	opennova::world::ReadyFireSound value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::world::ReadyFireSound &p_value) { value_ = p_value; }
	const opennova::world::ReadyFireSound &value() const { return value_; }
	static Ref<FireSoundRow> make(const String &p_soundset, const Vector3 &p_pos,
			int p_source_bms_id = 0);

	String get_soundset() const;
	Vector3 get_pos() const;
	int get_source_bms_id() const { return value_.source_bms_id; }
};

// One body slot-sound emission (world::SoundSlotEvent): the resolved
// SndProf.def set name, the foot-level position, the emitting entity's
// handle and the profile slot (43/44 refire every body tick).
class SlotSoundRow : public RefCounted {
	GDCLASS(SlotSoundRow, RefCounted)

	opennova::world::SoundSlotEvent value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::world::SoundSlotEvent &p_value) { value_ = p_value; }
	const opennova::world::SoundSlotEvent &value() const { return value_; }
	static Ref<SlotSoundRow> make(const String &p_soundset, const Vector3 &p_pos, int p_handle,
			int p_slot);

	String get_soundset() const;
	Vector3 get_pos() const;
	int get_handle() const { return static_cast<int>(value_.source_handle); }
	int get_slot() const { return static_cast<int>(value_.slot); }
};

// One persistent entity-attached emitter registration
// (world::SoundEmitterEvent): the keyed (source_spawn_id, lane) intent the
// audio layer expands into LWF layers. `source_only` refreshes the source
// pose without re-registering; a zero pitch or volume releases the lane.
class SoundEmitterRow : public RefCounted {
	GDCLASS(SoundEmitterRow, RefCounted)

	opennova::world::SoundEmitterEvent value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::world::SoundEmitterEvent &p_value) { value_ = p_value; }
	const opennova::world::SoundEmitterEvent &value() const { return value_; }
	static Ref<SoundEmitterRow> make(int64_t p_source_spawn_id, int p_handle, int p_source_bms_id,
			const Vector3 &p_pos, int p_lane, int p_slot, int p_lifetime, int64_t p_emitted_tick,
			int p_pitch_q16, int p_volume_q8_8, bool p_source_only, const String &p_soundset);

	int64_t get_source_spawn_id() const { return static_cast<int64_t>(value_.source_spawn_id); }
	int get_handle() const { return static_cast<int>(value_.source_handle); }
	int get_source_bms_id() const { return value_.source_bms_id; }
	Vector3 get_pos() const;
	int get_lane() const { return static_cast<int>(value_.lane); }
	int get_slot() const { return static_cast<int>(value_.slot); }
	int get_lifetime() const { return static_cast<int>(value_.lifetime_ticks); }
	int64_t get_emitted_tick() const { return static_cast<int64_t>(value_.emitted_tick); }
	int get_pitch_q16() const { return value_.pitch_q16; }
	int get_volume_q8_8() const { return static_cast<int>(value_.volume_q8_8); }
	bool get_source_only() const { return value_.source_only; }
	String get_soundset() const;
};

// One resolved round impact (Simulation::drain_round_impacts;
// world::RoundImpactPresentation carries the field notes).
class RoundImpactRow : public RefCounted {
	GDCLASS(RoundImpactRow, RefCounted)

	opennova::world::RoundImpactPresentation value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::world::RoundImpactPresentation &p_value) { value_ = p_value; }
	const opennova::world::RoundImpactPresentation &value() const { return value_; }

	Vector3 get_position() const;
	Vector3 get_direction() const;
	String get_effect() const;
	String get_sound() const;
	int64_t get_age_ticks() const { return static_cast<int64_t>(value_.age_ticks); }
	int64_t get_source_tick() const { return static_cast<int64_t>(value_.source_tick); }
	int64_t get_source_order() const { return static_cast<int64_t>(value_.source_order); }
	bool get_has_light() const { return value_.has_light; }
	float get_light_radius() const { return value_.light_radius; }
	Color get_light_color() const;
	int get_light_ticks() const { return value_.light_ticks; }
};

// One folded S2C 0x14 player-chat line (Simulation::drain_chat_lines;
// replication::ClientChatLine), routed by the witnessed channel table
// (hud/feed_format.h): sink 0 = the SYSTEM ring, 1 = the CHAT ring, 2 = the
// message queue, 3 = channel 3; `argb` is the channel's line color.
class ChatLineRow : public RefCounted {
	GDCLASS(ChatLineRow, RefCounted)

	opennova::replication::ClientChatLine value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::replication::ClientChatLine &p_value) { value_ = p_value; }

	String get_text() const;
	int64_t get_argb() const;
	int get_sink() const;
	int get_channel() const { return static_cast<int>(value_.channel); }
};

// One live death piece (world::DeathPieceRow; the trail effect is the debris
// type's row in the ONE native table, death_piece_trail_effect).
class DeathPieceRow : public RefCounted {
	GDCLASS(DeathPieceRow, RefCounted)

	opennova::world::DeathPieceRow value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::world::DeathPieceRow &p_value) { value_ = p_value; }
	const opennova::world::DeathPieceRow &value() const { return value_; }
	// The test constructor (the destruction pass's data leg).
	static Ref<DeathPieceRow> make(int p_slot, int64_t p_generation, int p_type_index,
			const Vector3 &p_pos, bool p_settled = false, int p_item_id = 0, int p_section = 0,
			float p_scale = 1.0f, float p_heading = 0.0f, float p_pitch = 0.0f);

	int get_slot() const { return value_.slot; }
	int64_t get_generation() const { return static_cast<int64_t>(value_.generation); }
	int get_item_id() const { return value_.item_id; }
	int get_section() const { return value_.section; }
	int get_type_index() const { return value_.type_index; }
	String get_trail() const;
	float get_scale() const { return value_.scale; }
	Vector3 get_pos() const;
	float get_heading() const { return value_.heading; }
	float get_pitch() const { return value_.pitch; }
	bool get_settled() const { return value_.settled; }
};

// One in-flight round glow (world::RoundGlowRow).
class RoundGlowRow : public RefCounted {
	GDCLASS(RoundGlowRow, RefCounted)

	opennova::world::RoundGlowRow value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::world::RoundGlowRow &p_value) { value_ = p_value; }
	const opennova::world::RoundGlowRow &value() const { return value_; }
	// The test constructor (EffectLightDirector.sync_round_glows' data leg).
	static Ref<RoundGlowRow> make(int64_t p_id, const Vector3 &p_pos, float p_radius,
			const Color &p_color);

	int64_t get_id() const { return static_cast<int64_t>(value_.id); }
	Vector3 get_pos() const;
	float get_radius() const { return value_.radius; }
	Color get_color() const;
};

// One World EffectLog entry (Simulation::drain_effects; world::Effect): the
// presentation-only side effects of the mission tick ("text", "dialog",
// "win", "lose", "round_end", "subgoal_won", "fx2ssn", the
// vehicle_control_* lifecycle edges, ...). `kind` selects the meaning of
// a..d and `text`; `wire_handle` is the drain's alias of d for the
// vehicle_control_* kinds (-1 otherwise), carried beside the engine value.
class MissionEffect : public RefCounted {
	GDCLASS(MissionEffect, RefCounted)

	opennova::world::Effect value_;
	int wire_handle_ = -1;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::world::Effect &p_value, int p_wire_handle) {
		value_ = p_value;
		wire_handle_ = p_wire_handle;
	}
	// The test/fixture constructor: kind plus the positional words and text.
	static Ref<MissionEffect> make(const String &p_kind, int p_a = 0, int p_b = 0, int p_c = 0,
			const String &p_text = String(), int p_d = 0, int p_wire_handle = -1);

	String get_kind() const;
	int get_a() const { return value_.a; }
	int get_b() const { return value_.b; }
	int get_c() const { return value_.c; }
	int get_d() const { return value_.d; }
	int get_wire_handle() const { return wire_handle_; }
	String get_text() const;
};

} // namespace godot
