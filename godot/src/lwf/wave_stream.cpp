#include "lwf/wave_stream.h"

#include <godot_cpp/core/class_db.hpp>

namespace godot {

void WaveStream::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_loader_pitch_q16", "pitch_q16"), &WaveStream::set_loader_pitch_q16);
	ClassDB::bind_method(D_METHOD("get_loader_pitch_q16"), &WaveStream::get_loader_pitch_q16);
	ClassDB::bind_method(D_METHOD("set_wave_rate", "rate"), &WaveStream::set_wave_rate);
	ClassDB::bind_method(D_METHOD("get_wave_rate"), &WaveStream::get_wave_rate);
	// Stored properties, so a duplicate (SoundBank::loop_copy) carries them.
	ADD_PROPERTY(PropertyInfo(Variant::INT, "loader_pitch_q16"), "set_loader_pitch_q16",
			"get_loader_pitch_q16");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "wave_rate"), "set_wave_rate", "get_wave_rate");
}

void WaveStream::set_loader_pitch_q16(int64_t p_pitch_q16) {
	loader_pitch_q16_ = p_pitch_q16;
}

int64_t WaveStream::get_loader_pitch_q16() const {
	return loader_pitch_q16_;
}

void WaveStream::set_wave_rate(int64_t p_rate) {
	wave_rate_ = p_rate;
}

int64_t WaveStream::get_wave_rate() const {
	return wave_rate_;
}

} // namespace godot
