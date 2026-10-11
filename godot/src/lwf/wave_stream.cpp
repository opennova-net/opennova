#include "lwf/wave_stream.h"

#include <godot_cpp/core/class_db.hpp>

namespace godot {

void WaveStream::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_loader_pitch_q16", "pitch_q16"), &WaveStream::set_loader_pitch_q16);
	ClassDB::bind_method(D_METHOD("get_loader_pitch_q16"), &WaveStream::get_loader_pitch_q16);
	// A stored property, so a duplicate (SoundBank::loop_copy) carries it.
	ADD_PROPERTY(PropertyInfo(Variant::INT, "loader_pitch_q16"), "set_loader_pitch_q16",
			"get_loader_pitch_q16");
}

void WaveStream::set_loader_pitch_q16(int64_t p_pitch_q16) {
	loader_pitch_q16_ = p_pitch_q16;
}

int64_t WaveStream::get_loader_pitch_q16() const {
	return loader_pitch_q16_;
}

} // namespace godot
