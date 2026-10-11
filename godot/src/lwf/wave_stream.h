#pragma once

#include <godot_cpp/classes/audio_stream_wav.hpp>

#include <cstdint>

namespace godot {

// A decoded wave's stream: an AudioStreamWAV that carries the game's wave loader pitch word
// (opennova::lwf::WavPcm::loader_pitch_q16, Q16) its decode recorded, its whole mix rate the wave's
// own rate up to INT32_MAX (WavPcm::sample_rate). Every player of a decoded wave sets the pitch scale
// WavLoader::pitch_scale_for gives over the one its voice composes, which reads the word and the mix
// rate (opennova::lwf::wave_pitch_scale: the wave plays at the mixer's step, a whole number of 1/512
// samples a device frame, a step of 0 at the least step). A stream that is not a WaveStream keeps
// the composed scale. Only WavLoader::from_pcm builds one; a WaveStream built otherwise keeps the
// default word 0x10000, which a play factor of 1 steps at 512, 44100 Hz, whatever its mix rate (no
// such caller exists).
class WaveStream : public AudioStreamWAV {
	GDCLASS(WaveStream, AudioStreamWAV);

protected:
	static void _bind_methods();

public:
	void set_loader_pitch_q16(int64_t p_pitch_q16);
	int64_t get_loader_pitch_q16() const;

private:
	int64_t loader_pitch_q16_ = 0x10000;
};

} // namespace godot
