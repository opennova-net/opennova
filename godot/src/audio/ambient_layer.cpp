#include "audio/ambient_layer.h"

using namespace godot;

void AmbientLayer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_wav_path"), &AmbientLayer::get_wav_path);
	ClassDB::bind_method(D_METHOD("set_wav_path", "value"), &AmbientLayer::set_wav_path);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "wav_path"), "set_wav_path", "get_wav_path");
	ClassDB::bind_method(D_METHOD("get_falloff_radius"), &AmbientLayer::get_falloff_radius);
	ClassDB::bind_method(D_METHOD("set_falloff_radius", "value"), &AmbientLayer::set_falloff_radius);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "falloff_radius"), "set_falloff_radius", "get_falloff_radius");
	ClassDB::bind_method(D_METHOD("get_min_distance"), &AmbientLayer::get_min_distance);
	ClassDB::bind_method(D_METHOD("set_min_distance", "value"), &AmbientLayer::set_min_distance);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "min_distance"), "set_min_distance", "get_min_distance");
	ClassDB::bind_method(D_METHOD("get_volume"), &AmbientLayer::get_volume);
	ClassDB::bind_method(D_METHOD("set_volume", "value"), &AmbientLayer::set_volume);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "volume"), "set_volume", "get_volume");
	ClassDB::bind_method(D_METHOD("get_clamp_volume"), &AmbientLayer::get_clamp_volume);
	ClassDB::bind_method(D_METHOD("set_clamp_volume", "value"), &AmbientLayer::set_clamp_volume);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "clamp_volume"), "set_clamp_volume", "get_clamp_volume");
	ClassDB::bind_method(D_METHOD("get_base_pitch"), &AmbientLayer::get_base_pitch);
	ClassDB::bind_method(D_METHOD("set_base_pitch", "value"), &AmbientLayer::set_base_pitch);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "base_pitch"), "set_base_pitch", "get_base_pitch");
	ClassDB::bind_method(D_METHOD("get_candidate_id"), &AmbientLayer::get_candidate_id);
	ClassDB::bind_method(D_METHOD("set_candidate_id", "value"), &AmbientLayer::set_candidate_id);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "candidate_id"), "set_candidate_id", "get_candidate_id");
	ClassDB::bind_method(D_METHOD("get_stream"), &AmbientLayer::get_stream);
	ClassDB::bind_method(D_METHOD("set_stream", "value"), &AmbientLayer::set_stream);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "stream", PROPERTY_HINT_RESOURCE_TYPE, "AudioStreamWAV"),
			"set_stream", "get_stream");
}
