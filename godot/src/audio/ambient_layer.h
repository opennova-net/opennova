#pragma once

#include <godot_cpp/classes/audio_stream_wav.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

#include <runtime/audio/volume_law.h>

namespace godot {

// One LWF layer as ambient candidate data (SoundBank.describe_ambient's row,
// the former layer-descriptor Dictionary, ADR 0017): member 0's wave and its
// volume / clamp (its pitch is no play factor, D-SND-56), the layer's two
// radii, the stable `candidate_id` the mission audio stamps for the mission's
// lifetime, and an injected `stream` a test hands in place of a resolved wave
// (read before the bank's resolve). Read-write so a test authors one.
class AmbientLayer : public RefCounted {
	GDCLASS(AmbientLayer, RefCounted)

	String wav_path_;
	int falloff_radius_ = 0;
	int min_distance_ = 0;
	int volume_ = opennova::audio::kVolumeByteMax;
	int clamp_volume_ = opennova::audio::kVolumeByteMax;
	int candidate_id_ = 0;
	Ref<AudioStreamWAV> stream_;

protected:
	static void _bind_methods();

public:
	String get_wav_path() const { return wav_path_; }
	void set_wav_path(const String &p_value) { wav_path_ = p_value; }
	int get_falloff_radius() const { return falloff_radius_; }
	void set_falloff_radius(int p_value) { falloff_radius_ = p_value; }
	int get_min_distance() const { return min_distance_; }
	void set_min_distance(int p_value) { min_distance_ = p_value; }
	int get_volume() const { return volume_; }
	void set_volume(int p_value) { volume_ = p_value; }
	int get_clamp_volume() const { return clamp_volume_; }
	void set_clamp_volume(int p_value) { clamp_volume_ = p_value; }
	// 0 = not yet stamped (ids start at 1).
	int get_candidate_id() const { return candidate_id_; }
	void set_candidate_id(int p_value) { candidate_id_ = p_value; }
	Ref<AudioStreamWAV> get_stream() const { return stream_; }
	void set_stream(const Ref<AudioStreamWAV> &p_value) { stream_ = p_value; }
};

} // namespace godot
