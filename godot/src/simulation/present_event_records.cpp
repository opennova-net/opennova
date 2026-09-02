#include "simulation/present_event_records.h"

using namespace godot;

namespace {

template <typename T>
constexpr Variant::Type variant_type_of();
template <>
constexpr Variant::Type variant_type_of<bool>() { return Variant::BOOL; }
template <>
constexpr Variant::Type variant_type_of<int>() { return Variant::INT; }
template <>
constexpr Variant::Type variant_type_of<int64_t>() { return Variant::INT; }
template <>
constexpr Variant::Type variant_type_of<float>() { return Variant::FLOAT; }
template <>
constexpr Variant::Type variant_type_of<String>() { return Variant::STRING; }
template <>
constexpr Variant::Type variant_type_of<Vector3>() { return Variant::VECTOR3; }
template <>
constexpr Variant::Type variant_type_of<Color>() { return Variant::COLOR; }

} // namespace

// One read-write property per field of the named record.
#define PRESENT_RECORD_BIND(m_class, m_fields)                                                      \
	void m_class::_bind_methods() {                                                                 \
		m_fields(PRESENT_RECORD_BIND_FIELD)                                                         \
	}
#define PRESENT_RECORD_BIND_FIELD(m_type, m_name, m_default)                                        \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &self_type::get_##m_name);                       \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &self_type::set_##m_name);              \
	ADD_PROPERTY(PropertyInfo(variant_type_of<m_type>(), #m_name), "set_" #m_name, "get_" #m_name);

PRESENT_RECORD_BIND(ThrowableVisualRow, THROWABLE_VISUAL_ROW_FIELDS)
PRESENT_RECORD_BIND(FirePresentationEvent, FIRE_PRESENTATION_EVENT_FIELDS)
PRESENT_RECORD_BIND(FireSoundRow, FIRE_SOUND_ROW_FIELDS)
PRESENT_RECORD_BIND(SlotSoundRow, SLOT_SOUND_ROW_FIELDS)
PRESENT_RECORD_BIND(SoundEmitterRow, SOUND_EMITTER_ROW_FIELDS)
PRESENT_RECORD_BIND(RoundImpactRow, ROUND_IMPACT_ROW_FIELDS)
PRESENT_RECORD_BIND(TerrainScorchRow, TERRAIN_SCORCH_ROW_FIELDS)
PRESENT_RECORD_BIND(WeatherSoundRow, WEATHER_SOUND_ROW_FIELDS)
PRESENT_RECORD_BIND(ChatLineRow, CHAT_LINE_ROW_FIELDS)
PRESENT_RECORD_BIND(ObjectiveRow, OBJECTIVE_ROW_FIELDS)
PRESENT_RECORD_BIND(DeathPieceRow, DEATH_PIECE_ROW_FIELDS)
PRESENT_RECORD_BIND(RoundGlowRow, ROUND_GLOW_ROW_FIELDS)

void MissionEffect::_bind_methods() {
	MISSION_EFFECT_FIELDS(PRESENT_RECORD_BIND_FIELD)
	ClassDB::bind_static_method("MissionEffect",
			D_METHOD("make", "kind", "a", "b", "c", "text"), &MissionEffect::make,
			DEFVAL(0), DEFVAL(0), DEFVAL(0), DEFVAL(String()));
}

Ref<MissionEffect> MissionEffect::make(const String &p_kind, int p_a, int p_b, int p_c,
		const String &p_text) {
	Ref<MissionEffect> out;
	out.instantiate();
	out->set_kind(p_kind);
	out->set_a(p_a);
	out->set_b(p_b);
	out->set_c(p_c);
	out->set_text(p_text);
	return out;
}

#undef PRESENT_RECORD_BIND_FIELD
#undef PRESENT_RECORD_BIND
