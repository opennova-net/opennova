#include "audio/mission_audio_records.h"

#include <godot_cpp/core/object.hpp>

using namespace godot;

// ------------------------------------------------------------ MissionAudioMarker

void MissionAudioMarker::set_layers(const String &p_set_name, const TypedArray<AmbientLayer> &p_layers) {
	const int existing = set_names_.find(p_set_name);
	if (existing >= 0) {
		set_layers_.write[existing] = p_layers;
		return;
	}
	set_names_.push_back(p_set_name);
	set_layers_.push_back(p_layers);
}

PackedStringArray MissionAudioMarker::get_set_names() const {
	PackedStringArray out;
	for (const String &name : set_names_) {
		out.push_back(name);
	}
	return out;
}

TypedArray<AmbientLayer> MissionAudioMarker::get_layers(const String &p_set_name) const {
	const int existing = set_names_.find(p_set_name);
	return existing >= 0 ? set_layers_[existing] : TypedArray<AmbientLayer>();
}

void MissionAudioMarker::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_pos"), &MissionAudioMarker::get_pos);
	ClassDB::bind_method(D_METHOD("set_pos", "value"), &MissionAudioMarker::set_pos);
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "pos"), "set_pos", "get_pos");
	ClassDB::bind_method(D_METHOD("get_source_bms_id"), &MissionAudioMarker::get_source_bms_id);
	ClassDB::bind_method(D_METHOD("set_source_bms_id", "value"), &MissionAudioMarker::set_source_bms_id);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "source_bms_id"), "set_source_bms_id", "get_source_bms_id");
	ClassDB::bind_method(D_METHOD("get_slot_sets"), &MissionAudioMarker::get_slot_sets);
	ClassDB::bind_method(D_METHOD("set_slot_sets", "value"), &MissionAudioMarker::set_slot_sets);
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "slot_sets"), "set_slot_sets", "get_slot_sets");
	ClassDB::bind_method(D_METHOD("get_stagger_slot"), &MissionAudioMarker::get_stagger_slot);
	ClassDB::bind_method(D_METHOD("set_stagger_slot", "value"), &MissionAudioMarker::set_stagger_slot);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "stagger_slot"), "set_stagger_slot", "get_stagger_slot");
	ClassDB::bind_method(D_METHOD("set_layers", "set_name", "layers"), &MissionAudioMarker::set_layers);
	ClassDB::bind_method(D_METHOD("get_set_names"), &MissionAudioMarker::get_set_names);
	ClassDB::bind_method(D_METHOD("get_layers", "set_name"), &MissionAudioMarker::get_layers);
}

// ----------------------------------------------------------- MissionAudioChannel

AudioStreamPlayer3D *MissionAudioChannel::get_player() const {
	return Object::cast_to<AudioStreamPlayer3D>(ObjectDB::get_instance(player_id_));
}

void MissionAudioChannel::set_player(AudioStreamPlayer3D *p_player) {
	player_id_ = p_player != nullptr ? ObjectID(p_player->get_instance_id()) : ObjectID();
}

void MissionAudioChannel::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_player"), &MissionAudioChannel::get_player);
	ClassDB::bind_method(D_METHOD("set_player", "player"), &MissionAudioChannel::set_player);
	ClassDB::bind_method(D_METHOD("get_candidate_id"), &MissionAudioChannel::get_candidate_id);
	ClassDB::bind_method(D_METHOD("set_candidate_id", "value"), &MissionAudioChannel::set_candidate_id);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "candidate_id"), "set_candidate_id", "get_candidate_id");
}

// -------------------------------------------------- MissionAudioCandidateBinding

void MissionAudioCandidateBinding::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_descriptor"), &MissionAudioCandidateBinding::get_descriptor);
	ClassDB::bind_method(D_METHOD("set_descriptor", "value"), &MissionAudioCandidateBinding::set_descriptor);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "descriptor", PROPERTY_HINT_RESOURCE_TYPE, "AmbientLayer"),
			"set_descriptor", "get_descriptor");
	ClassDB::bind_method(D_METHOD("get_bus"), &MissionAudioCandidateBinding::get_bus);
	ClassDB::bind_method(D_METHOD("set_bus", "value"), &MissionAudioCandidateBinding::set_bus);
	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "bus"), "set_bus", "get_bus");
}

// ---------------------------------------------------- MissionAudioDynamicEmitter

void MissionAudioDynamicEmitter::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_set_name"), &MissionAudioDynamicEmitter::get_set_name);
	ClassDB::bind_method(D_METHOD("set_set_name", "value"), &MissionAudioDynamicEmitter::set_set_name);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "set_name"), "set_set_name", "get_set_name");
	ClassDB::bind_method(D_METHOD("get_candidate_ids"), &MissionAudioDynamicEmitter::get_candidate_ids);
	ClassDB::bind_method(D_METHOD("set_candidate_ids", "value"), &MissionAudioDynamicEmitter::set_candidate_ids);
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "candidate_ids"), "set_candidate_ids", "get_candidate_ids");
	ClassDB::bind_method(D_METHOD("get_expires_tick"), &MissionAudioDynamicEmitter::get_expires_tick);
	ClassDB::bind_method(D_METHOD("set_expires_tick", "value"), &MissionAudioDynamicEmitter::set_expires_tick);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "expires_tick"), "set_expires_tick", "get_expires_tick");
}

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
