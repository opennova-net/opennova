// Streaming AudioStreamPlayback for one entry inside a NovaSbfBank.
//
// Witnessed engine equivalents (jointops.exe):
//   - Sbf_StartEntry          @ 0x004ED910: per-entry block_size /
//                                            data_offset / total_size init.
//   - Audio_StreamNextChunk   @ 0x004ED7D0: per-tick chunk read + decode.
//   - sbf_decode_chunk (libs): algebraically equivalent to the engine's
//                              Audio_SubmitStereoSampleSplit @ 0x007BD205
//                              at unity gain (see sbf_ida_witness.md).
//
// Chunk data is read from NovaSbfBank's decoded in-memory byte stream, so
// SCR-wrapped loose banks and plaintext banks use the same playback path.

#include "nova_sbf_audio_stream_playback.h"

#include "nova_sbf_bank.h"

#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace {

// One chunk holds at most SBF_CHUNK_AUDIO int16 values where alternating
// values are L,R,L,R,... so SBF_CHUNK_AUDIO / 2 stereo frames per chunk.
constexpr uint32_t kFramesPerChunk = SBF_CHUNK_AUDIO / 2;

} // namespace

void NovaSbfAudioStreamPlayback::bind_to(const Ref<NovaSbfBank> &p_bank, int p_entry_index) {
	_bank = p_bank;
	_entry_index = p_entry_index;
}

void NovaSbfAudioStreamPlayback::_start(double p_from_pos) {
	if (_bank.is_null()) {
		_playing = false;
		return;
	}
	_playing = _bank->raw_entry_at(_entry_index) != nullptr;
	_current_chunk = -1; // force reload on first mix
	_decoded_count = 0;
	_frames_consumed = (uint64_t)(p_from_pos * (double)SBF_SAMPLE_RATE);
	begin_resample();
}

void NovaSbfAudioStreamPlayback::_stop() {
	_playing = false;
}

bool NovaSbfAudioStreamPlayback::_is_playing() const {
	return _playing;
}

void NovaSbfAudioStreamPlayback::_seek(double p_position) {
	_frames_consumed = (uint64_t)(p_position * (double)SBF_SAMPLE_RATE);
	_current_chunk = -1; // force reload on next mix
	_decoded_count = 0;
	begin_resample();
}

bool NovaSbfAudioStreamPlayback::_load_chunk(int p_chunk_index) {
	const SbfRawEntry *e = _bank.is_valid() ? _bank->raw_entry_at(_entry_index) : nullptr;
	if (!e || e->block_size == 0) {
		_decoded_count = 0;
		return false;
	}

	const uint64_t chunk_byte_off = (uint64_t)p_chunk_index * (uint64_t)e->block_size;
	if (chunk_byte_off >= e->total_size) {
		// Past end of entry. Engine equivalent: dword_C60D8C -= block_size in
		// Audio_StreamNextChunk drops to 0 and the playback loop terminates.
		_decoded_count = 0;
		return false;
	}

	const uint64_t off = (uint64_t)e->data_offset + chunk_byte_off;
	PackedByteArray block;
	if (!_bank->read_file_block(off, e->block_size, block)) {
		_decoded_count = 0;
		return false;
	}

	// sbf_decode_chunk honours the chunk header's valid_samples (count of
	// AUDIO BYTES, not int16s) and produces interleaved L,R,L,R,... int16s.
	int n = sbf_decode_chunk(block.ptr(), (size_t)block.size(),
			_decoded, SBF_CHUNK_AUDIO);
	if (n < 0) {
		_decoded_count = 0;
		return false;
	}
	_decoded_count = (uint32_t)n;
	_current_chunk = p_chunk_index;
	return true;
}

float NovaSbfAudioStreamPlayback::_get_stream_sampling_rate() const {
	return (float)SBF_SAMPLE_RATE;
}

int32_t NovaSbfAudioStreamPlayback::_mix_resampled(AudioFrame *p_buffer,
		int32_t p_frames) {
	// AudioStreamPlaybackResampled uses frames beyond the returned count as
	// cubic-interpolation lookahead. Keep that tail deterministic even when an
	// invalid entry or EOF makes us return fewer frames than requested.
	for (int32_t i = 0; i < p_frames; ++i) {
		p_buffer[i].left = 0.0f;
		p_buffer[i].right = 0.0f;
	}

	if (!_playing) {
		return 0;
	}
	if (_bank.is_null()) {
		_playing = false;
		return 0;
	}
	const SbfRawEntry *e = _bank->raw_entry_at(_entry_index);
	if (!e || e->block_size == 0) {
		_playing = false;
		return 0;
	}

	// Per Phase A IDA witness (sbf_ida_witness.md), SBF audio is byte-paired
	// stereo: each chunk produces valid_samples int16 values, alternating
	// L,R,L,R,... sbf_decode_chunk emits them already interleaved. One stereo
	// frame = 2 int16 values.
	int32_t written = 0;
	while (written < p_frames) {
		const uint64_t frame_pos = _frames_consumed;
		const int needed_chunk = (int)(frame_pos / (uint64_t)kFramesPerChunk);

		if (needed_chunk != _current_chunk) {
			if (!_load_chunk(needed_chunk)) {
				_playing = false;
				return written;
			}
		}

		const uint32_t local_frame = (uint32_t)(frame_pos - (uint64_t)needed_chunk * (uint64_t)kFramesPerChunk);
		const uint32_t local_int16 = local_frame * 2u;
		// _decoded_count == 0 covers EOF; +1 ensures the R sample is also in
		// range. Partial last chunks fall out here when the playhead runs
		// past valid_samples.
		if (local_int16 + 1u >= _decoded_count) {
			_playing = false;
			return written;
		}

		const int16_t l16 = _decoded[local_int16 + 0];
		const int16_t r16 = _decoded[local_int16 + 1];
		p_buffer[written].left = (float)l16 / 32768.0f;
		p_buffer[written].right = (float)r16 / 32768.0f;
		++written;
		++_frames_consumed;
	}
	return written;
}

double NovaSbfAudioStreamPlayback::_get_playback_position() const {
	return (double)_frames_consumed / (double)SBF_SAMPLE_RATE;
}
