#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

#include <runtime/audio/volume_law.h>

namespace godot {

// One LWF layer as ambient candidate data (SoundBank.describe_ambient's row,
// the GDScript form of the engine's audio::AmbientLayerRow, ADR 0017): member
// 0's wave and its volume / clamp (its pitch is no play factor, D-SND-56) and
// the layer's two radii. Read-write so a test authors one. The mission's
// ambient emitters take the engine rows themselves (SoundBank::ambient_layer_rows).
class AmbientLayer : public RefCounted {
	GDCLASS(AmbientLayer, RefCounted)

	String wav_path_;
	int falloff_radius_ = 0;
	int min_distance_ = 0;
	int volume_ = opennova::audio::kVolumeByteMax;
	int clamp_volume_ = opennova::audio::kVolumeByteMax;

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
};

} // namespace godot
