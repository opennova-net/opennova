#include "audio/mission_audio_records.h"

#include <godot_cpp/core/object.hpp>

using namespace godot;

// ------------------------------------------------------------- MissionAudioStats

Dictionary MissionAudioStats::to_json_value() const {
	Dictionary out;
#define MISSION_AUDIO_STATS_JSON(m_name, m_default) out[#m_name] = m_name##_;
	MISSION_AUDIO_STATS_FIELDS(MISSION_AUDIO_STATS_JSON)
#undef MISSION_AUDIO_STATS_JSON
	return out;
}

void MissionAudioStats::_bind_methods() {
#define MISSION_AUDIO_STATS_BIND(m_name, m_default)                                              \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &MissionAudioStats::get_##m_name);           \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &MissionAudioStats::set_##m_name);  \
	ADD_PROPERTY(PropertyInfo(Variant::INT, #m_name), "set_" #m_name, "get_" #m_name);
	MISSION_AUDIO_STATS_FIELDS(MISSION_AUDIO_STATS_BIND)
#undef MISSION_AUDIO_STATS_BIND
	ClassDB::bind_method(D_METHOD("to_json_value"), &MissionAudioStats::to_json_value);
}

// -------------------------------------------------------------- MissionAudioPerf

Dictionary MissionAudioPerf::to_json_value() const {
	Dictionary out;
#define MISSION_AUDIO_PERF_JSON(m_name) out[#m_name] = m_name##_;
	MISSION_AUDIO_PERF_FIELDS(MISSION_AUDIO_PERF_JSON)
#undef MISSION_AUDIO_PERF_JSON
	return out;
}

void MissionAudioPerf::_bind_methods() {
#define MISSION_AUDIO_PERF_BIND(m_name)                                                         \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &MissionAudioPerf::get_##m_name);            \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &MissionAudioPerf::set_##m_name);   \
	ADD_PROPERTY(PropertyInfo(Variant::INT, #m_name), "set_" #m_name, "get_" #m_name);
	MISSION_AUDIO_PERF_FIELDS(MISSION_AUDIO_PERF_BIND)
#undef MISSION_AUDIO_PERF_BIND
	ClassDB::bind_method(D_METHOD("to_json_value"), &MissionAudioPerf::to_json_value);
}

// ----------------------------------------------------------------- FiredSoundset

void FiredSoundset::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_set_name"), &FiredSoundset::get_set_name);
	ClassDB::bind_method(D_METHOD("set_set_name", "value"), &FiredSoundset::set_set_name);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "set_name"), "set_set_name", "get_set_name");
	ClassDB::bind_method(D_METHOD("get_position"), &FiredSoundset::get_position);
	ClassDB::bind_method(D_METHOD("set_position", "value"), &FiredSoundset::set_position);
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "position"), "set_position", "get_position");
	ClassDB::bind_method(D_METHOD("get_source_bms_id"), &FiredSoundset::get_source_bms_id);
	ClassDB::bind_method(D_METHOD("set_source_bms_id", "value"), &FiredSoundset::set_source_bms_id);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "source_bms_id"), "set_source_bms_id", "get_source_bms_id");
	ClassDB::bind_method(D_METHOD("is_slot"), &FiredSoundset::is_slot);
	ClassDB::bind_method(D_METHOD("set_slot", "value"), &FiredSoundset::set_slot);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "slot"), "set_slot", "is_slot");
	ClassDB::bind_method(D_METHOD("is_played"), &FiredSoundset::is_played);
	ClassDB::bind_method(D_METHOD("set_played", "value"), &FiredSoundset::set_played);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "played"), "set_played", "is_played");
}
