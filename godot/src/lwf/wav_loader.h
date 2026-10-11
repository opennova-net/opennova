#pragma once

#include <godot_cpp/classes/audio_stream_wav.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <formats/lwf/wav_pcm.h>

namespace godot {

// Decodes raw RIFF/WAVE bytes (as served by the VFS / ResourceRoot) into a
// Godot AudioStreamWAV, always emitting signed 16-bit (FORMAT_16_BITS):
//   - PCM 8-bit UNSIGNED (ambient, e.g. game.lwf): upconvert (u-128)<<8.
//   - PCM 16-bit signed LE: copied verbatim.
//   - IMA-ADPCM (audioFormat 0x11; NovaLogic voice / zone / dialog audio, 4-bit
//     block-based): decoded to 16-bit via the standard step/index tables.
// Other formats (e.g. MS-ADPCM 0x02) are unsupported and return a null Ref.
class WavLoader : public RefCounted {
	GDCLASS(WavLoader, RefCounted);

protected:
	static void _bind_methods();

public:
	// Build an AudioStreamWAV from RIFF/WAVE bytes. Returns null on parse error
	// or unsupported encoding. loop_mode/loop points are left at defaults; the
	// caller applies looping.
	static Ref<AudioStreamWAV> from_bytes(const PackedByteArray &p_bytes);
	// Box an engine-decoded clip without decoding/caching a second copy. The stream's mix rate is a
	// whole int32: a rate past INT32_MAX (an AUD1 pitch from 0xBE37C63A, a RIFF rate from 0x80000000)
	// is boxed at INT32_MAX, which every player's scale steps up to the wave's step (pitch_scale_for).
	static Ref<AudioStreamWAV> from_pcm(const opennova::lwf::WavPcm &decoded);
	// Every decoded stream is a WaveStream carrying its loader pitch word, and every player of one sets
	// the pitch scale this gives over the one its voice composes: the wave plays at the mixer's step, a
	// whole number of 1/512 samples a device frame of the 44100 Hz device (86.13 Hz each), a step of 0
	// (a wave of pitch 0 whatever the voice pitch, a play factor of 0 whatever the wave) at the least
	// step, a boxed rate at its step too (opennova::lwf::wave_pitch_scale carries the witness); any
	// other stream keeps `p_pitch_scale`.
	static double pitch_scale_for(const Ref<AudioStream> &p_stream, double p_pitch_scale);
};

} // namespace godot

