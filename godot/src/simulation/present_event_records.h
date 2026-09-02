#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>

// The per-tick presentation drains Simulation hands the GDScript present
// passes: one typed RefCounted record per event kind (ADR 0042 d5), read-write
// so a test authors one. Every position is Godot space (mission (x, y, z) ->
// (x, z, -y)); the witnesses live on the engine event each drain reads.
// Each field list is an X-macro of (type, name, default) so the accessors,
// members and bindings are generated once (present_event_records.cpp).

#define PRESENT_RECORD_ACCESSORS(m_type, m_name, m_default)   \
	m_type get_##m_name() const { return m_name##_; }        \
	void set_##m_name(m_type p_value) { m_name##_ = p_value; }
#define PRESENT_RECORD_MEMBER(m_type, m_name, m_default) m_type m_name##_ = m_default;

#define PRESENT_RECORD_CLASS(m_class, m_fields)          \
	class m_class : public RefCounted {                  \
		GDCLASS(m_class, RefCounted)                     \
                                                         \
	public:                                              \
		m_fields(PRESENT_RECORD_ACCESSORS)               \
                                                         \
	protected:                                           \
		static void _bind_methods();                     \
                                                         \
	private:                                             \
		m_fields(PRESENT_RECORD_MEMBER)                  \
	};

namespace godot {

// One item-modeled throwable per frame (Simulation::get_throwable_visuals):
// a tracer-cadence flying round with a TrcrID model or a placed device.
// `key` is generation<<10 | pool slot for rounds and 0x40.. | entity for
// devices, so a same-slot replacement never inherits the outgoing model or
// effect group; `rotation_deg` is the placer euler (pitch, mission yaw, roll).
// `move_effect` is the effects_table tag-1 round-bound particle and
// `move_effect_live` its emitter liveness (the round+0x1CC handle mirror).
#define THROWABLE_VISUAL_ROW_FIELDS(X)  \
	X(int64_t, key, 0)                  \
	X(int, item_id, 0)                  \
	X(Vector3, pos, Vector3())          \
	X(Vector3, rotation_deg, Vector3()) \
	X(String, move_effect, String())    \
	X(bool, move_effect_live, true)

PRESENT_RECORD_CLASS(ThrowableVisualRow, THROWABLE_VISUAL_ROW_FIELDS)

// One round spawned since the last drain
// (Simulation::drain_fire_presentation_events): the EFFECT legs of both
// retail receive arms. `adm_arm` selects the adm-indexed arm, whose
// `action_effect` spawns at the addressed weapon's `action_userpoint` on
// the rendered gun; the ammo arm spawns `effect` at `origin` along `forward`.
#define FIRE_PRESENTATION_EVENT_FIELDS(X)   \
	X(Vector3, origin, Vector3())           \
	X(bool, adm_arm, false)                 \
	X(int, adm_index, 0)                    \
	X(Vector3, forward, Vector3(0, 0, -1))  \
	X(int, shooter_handle, -1)              \
	X(int, source_bms_id, 0)                \
	X(bool, is_local_player, false)         \
	X(int, ammo_index, 0)                   \
	X(String, effect, String())             \
	X(int, mf_light, 0)                     \
	X(String, action_effect, String())      \
	X(String, action_userpoint, String())

PRESENT_RECORD_CLASS(FirePresentationEvent, FIRE_PRESENTATION_EVENT_FIELDS)

// One ready fire one-shot on the logic clock (Simulation::drain_fire_sounds;
// world/fire_sound.h): the sound set name, its position and the source's
// bms id for the set's max-range cull.
#define FIRE_SOUND_ROW_FIELDS(X)     \
	X(String, soundset, String())    \
	X(Vector3, pos, Vector3())       \
	X(int, source_bms_id, 0)

PRESENT_RECORD_CLASS(FireSoundRow, FIRE_SOUND_ROW_FIELDS)

// One body slot-sound emission (Simulation::drain_slot_sounds): the
// resolved SndProf.def set name, the foot-level position, the emitting
// entity's handle and the profile slot (43/44 refire every body tick).
#define SLOT_SOUND_ROW_FIELDS(X)     \
	X(String, soundset, String())    \
	X(Vector3, pos, Vector3())       \
	X(int, handle, 0)                \
	X(int, slot, 0)

PRESENT_RECORD_CLASS(SlotSoundRow, SLOT_SOUND_ROW_FIELDS)

// One persistent entity-attached emitter registration
// (Simulation::drain_sound_emitters; world/sound_emitter_mailbox.h): the
// keyed (source_spawn_id, lane) intent the audio layer expands into LWF
// layers. `source_only` refreshes the source pose without re-registering;
// a zero pitch or volume releases the lane.
#define SOUND_EMITTER_ROW_FIELDS(X)   \
	X(int64_t, source_spawn_id, 0)    \
	X(int, handle, 0)                 \
	X(int, source_bms_id, 0)          \
	X(Vector3, pos, Vector3())        \
	X(int, lane, 0)                   \
	X(int, slot, 0)                   \
	X(int, lifetime, 30)              \
	X(int64_t, emitted_tick, 0)       \
	X(int, pitch_q16, 0)              \
	X(int, volume_q8_8, 0)            \
	X(bool, source_only, false)       \
	X(String, soundset, String())

PRESENT_RECORD_CLASS(SoundEmitterRow, SOUND_EMITTER_ROW_FIELDS)

// One resolved round impact (Simulation::drain_round_impacts), already
// mapped through the ammo effects_table to its effect and sound legs.
// `age_ticks` is the catch-up pre-age; `has_light` is the light_impact
// gate (the effect leg AND an authored radius), with the flash fields
// meaningful only when set.
#define ROUND_IMPACT_ROW_FIELDS(X)       \
	X(Vector3, position, Vector3())      \
	X(Vector3, direction, Vector3())     \
	X(String, effect, String())          \
	X(String, sound, String())           \
	X(int64_t, age_ticks, 0)             \
	X(int64_t, source_tick, 0)           \
	X(int64_t, source_order, 0)          \
	X(bool, has_light, false)            \
	X(float, light_radius, 0.0f)         \
	X(Color, light_color, Color(1, 1, 1)) \
	X(int, light_ticks, 10)

PRESENT_RECORD_CLASS(RoundImpactRow, ROUND_IMPACT_ROW_FIELDS)

// One permanent terrain scorch insertion (Simulation::drain_terrain_scorches):
// exact 16.16 terrain x/z bounds, already folded from mission (x, y).
#define TERRAIN_SCORCH_ROW_FIELDS(X) \
	X(int64_t, texture_index, -1)    \
	X(int64_t, minimum_x_q16, 0)     \
	X(int64_t, maximum_x_q16, 0)     \
	X(int64_t, minimum_z_q16, 0)     \
	X(int64_t, maximum_z_q16, 0)     \
	X(int64_t, source_tick, 0)       \
	X(int64_t, source_order, 0)

PRESENT_RECORD_CLASS(TerrainScorchRow, TERRAIN_SCORCH_ROW_FIELDS)

// One thunder one-shot (Simulation::drain_weather_sounds): the distance
// from the listener in metres and the bearing as a 0..255 turn (128 =
// behind the camera); weather_state.h carries the cites.
#define WEATHER_SOUND_ROW_FIELDS(X) \
	X(float, distance, 1.0f)        \
	X(int, bearing, 0)

PRESENT_RECORD_CLASS(WeatherSoundRow, WEATHER_SOUND_ROW_FIELDS)

// One folded S2C 0x14 player-chat line (Simulation::drain_chat_lines),
// routed by the witnessed channel table: sink 0 = the SYSTEM ring, 1 = the
// CHAT ring, 2 = the message queue, 3 = channel 3.
#define CHAT_LINE_ROW_FIELDS(X) \
	X(String, text, String())   \
	X(int64_t, argb, -1)        \
	X(int, sink, 0)             \
	X(int, channel, 0)

PRESENT_RECORD_CLASS(ChatLineRow, CHAT_LINE_ROW_FIELDS)

// One World EffectLog entry (Simulation::drain_effects): the presentation-
// only side effects of the mission tick ("text", "dialog", "win", "lose",
// "round_end", "subgoal_won", "fx2ssn", the vehicle_control_* lifecycle
// edges, ...). `kind` selects the meaning of a..d and `text`;
// `wire_handle` carries d for the vehicle_control_* kinds and is -1
// otherwise.
#define MISSION_EFFECT_FIELDS(X)  \
	X(String, kind, String())     \
	X(int, a, 0)                  \
	X(int, b, 0)                  \
	X(int, c, 0)                  \
	X(int, d, 0)                  \
	X(int, wire_handle, -1)       \
	X(String, text, String())

class MissionEffect : public RefCounted {
	GDCLASS(MissionEffect, RefCounted)

public:
	MISSION_EFFECT_FIELDS(PRESENT_RECORD_ACCESSORS)

	// The test/fixture constructor: kind plus the positional words and text.
	static Ref<MissionEffect> make(const String &p_kind, int p_a, int p_b, int p_c,
			const String &p_text);

protected:
	static void _bind_methods();

private:
	MISSION_EFFECT_FIELDS(PRESENT_RECORD_MEMBER)
};

} // namespace godot

#undef PRESENT_RECORD_CLASS
