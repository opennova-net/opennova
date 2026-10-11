#pragma once

#include <godot_cpp/classes/audio_stream_wav.hpp>

#include <cstdint>

namespace godot {

// A decoded wave's stream: an AudioStreamWAV that carries the game's wave loader pitch word
// (opennova::lwf::WavPcm::loader_pitch_q16, Q16) its decode recorded and the wave's own rate
// (WavPcm::sample_rate), which the stream's whole mix rate holds up to INT32_MAX. Every player of a
// decoded wave sets the pitch scale WavLoader::pitch_scale_for gives over the one its voice composes,
// which reads both (opennova::lwf::wave_pitch_scale: a step of 0, a wave of pitch 0 whatever the
// voice pitch or a play factor of 0 whatever the wave, plays at the mixer's least step, a rate boxed
// at INT32_MAX at its own). A stream that is not a WaveStream keeps the composed scale.
class WaveStream : public AudioStreamWAV {
	GDCLASS(WaveStream, AudioStreamWAV);

protected:
	static void _bind_methods();

public:
	void set_loader_pitch_q16(int64_t p_pitch_q16);
	int64_t get_loader_pitch_q16() const;
	// 0 (a stream made elsewhere): its mix rate.
	void set_wave_rate(int64_t p_rate);
	int64_t get_wave_rate() const;

private:
	int64_t loader_pitch_q16_ = 0x10000;
	int64_t wave_rate_ = 0;
};

} // namespace godot
