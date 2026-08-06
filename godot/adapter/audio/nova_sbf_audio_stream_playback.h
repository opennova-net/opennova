#ifndef NOVA_SBF_AUDIO_STREAM_PLAYBACK_H
#define NOVA_SBF_AUDIO_STREAM_PLAYBACK_H

#include <godot_cpp/classes/audio_frame.hpp>
#include <godot_cpp/classes/audio_stream_playback_resampled.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/variant.hpp>

#include "sbf/sbf.h"

#include "nova_sbf_bank.h"

namespace godot {

// Streaming playback for a single SBF entry. Chunk reads come from the bank's
// decoded in-memory bytes so loose SCR-wrapped banks play the same as plaintext.
//
// Resampled: SBF content is int16 stereo at SBF_SAMPLE_RATE (22050 Hz) while the
// mixer runs at the device rate (44100 by default), so the base class's resampler
// pulls source frames via _mix_resampled at the rate _get_stream_sampling_rate
// reports. (The original streams these buffers through DirectSound, which
// resamples to the device rate the same way; a 1:1 frame mapping played SBF
// content at half speed.)
//
// Hot path: _mix_resampled walks the requested frame_count one frame at a time,
// lazy loading the next chunk when the playhead crosses a chunk boundary. One
// chunk = block_size bytes on disk = up to SBF_CHUNK_AUDIO int16 values
// (interleaved L,R,L,R,...) = up to SBF_CHUNK_AUDIO / 2 stereo frames.
class NovaSbfAudioStreamPlayback : public AudioStreamPlaybackResampled {
	GDCLASS(NovaSbfAudioStreamPlayback, AudioStreamPlaybackResampled)

public:
	// Holds a strong Ref to the bank: mixing runs on the audio server thread and
	// reads entry data that lives inside the bank, so a raw pointer would dangle
	// if the bank were freed on the main thread while a mix was in flight.
	void bind_to(const Ref<NovaSbfBank> &p_bank, int p_entry_index);

	// AudioStreamPlaybackResampled overrides. Signatures match godot-cpp's
	// generated audio_stream_playback_resampled.hpp.
	void _start(double p_from_pos) override;
	void _stop() override;
	bool _is_playing() const override;
	void _seek(double p_position) override;
	int32_t _mix_resampled(AudioFrame *p_dst_buffer, int32_t p_frame_count) override;
	float _get_stream_sampling_rate() const override;
	double _get_playback_position() const override;

protected:
	static void _bind_methods() {}

private:
	bool _load_chunk(int p_chunk_index);

	Ref<NovaSbfBank> _bank;
	int _entry_index = -1;
	bool _playing = false;
	int _current_chunk = -1;
	int16_t _decoded[SBF_CHUNK_AUDIO];
	uint32_t _decoded_count = 0;
	uint64_t _frames_consumed = 0; // total stereo frames produced since _start
};

} // namespace godot

#endif // NOVA_SBF_AUDIO_STREAM_PLAYBACK_H
