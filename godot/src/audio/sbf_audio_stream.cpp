// AudioStream wrapper for a single entry inside a SbfBank.
// Witnessed engine equivalent: jointops!Sbf_StartEntry @ 0x004ED910 sets up
// per-entry streaming state (block_size / total_size / data_offset), which the
// entry's decode (sbf::sbf_decode_entry) reads via raw_entry_at().

#include "audio/sbf_audio_stream.h"

#include "audio/sbf_bank.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include <cstring>
#include <vector>

using namespace godot;
using namespace opennova::sbf;

void SbfAudioStream::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_index"), &SbfAudioStream::get_index);
	ClassDB::bind_method(D_METHOD("get_wav"), &SbfAudioStream::get_wav);
}

void SbfAudioStream::configure(SbfBank *p_bank, int p_entry_index) {
	_bank = Ref<SbfBank>(p_bank);
	_entry_index = p_entry_index;
}

Ref<AudioStreamWAV> SbfAudioStream::get_wav() const {
	if (_bank.is_null()) {
		return Ref<AudioStreamWAV>();
	}
	const SbfRawEntry *e = _bank->raw_entry_at(_entry_index);
	if (!e) {
		return Ref<AudioStreamWAV>();
	}
	PackedByteArray raw;
	if (!_bank->read_file_block(e->data_offset, e->total_size, raw)) {
		return Ref<AudioStreamWAV>();
	}
	const std::vector<int16_t> pcm = sbf_decode_entry(raw.ptr(), (size_t)raw.size(), e->block_size);
	PackedByteArray data;
	data.resize((int64_t)(pcm.size() * sizeof(int16_t)));
	if (!pcm.empty()) {
		std::memcpy(data.ptrw(), pcm.data(), pcm.size() * sizeof(int16_t));
	}
	Ref<AudioStreamWAV> wav;
	wav.instantiate();
	wav->set_format(AudioStreamWAV::FORMAT_16_BITS);
	wav->set_stereo(true);
	wav->set_mix_rate(SBF_SAMPLE_RATE);
	wav->set_data(data);
	return wav;
}

Ref<AudioStreamPlayback> SbfAudioStream::_instantiate_playback() const {
	// A built-in playback, never an extension one (the class comment says why).
	const Ref<AudioStreamWAV> wav = get_wav();
	if (wav.is_null()) {
		return Ref<AudioStreamPlayback>();
	}
	return wav->instantiate_playback();
}

String SbfAudioStream::_get_stream_name() const {
	if (_bank.is_null()) {
		return String();
	}
	const SbfRawEntry *e = _bank->raw_entry_at(_entry_index);
	if (!e) {
		return String();
	}
	// SbfRawEntry.name is null-padded to SBF_NAME_SIZE. Copy through a
	// guarded buffer so String() sees a real terminator.
	char name_buf[SBF_NAME_SIZE + 1] = { 0 };
	std::memcpy(name_buf, e->name, SBF_NAME_SIZE);
	return String(name_buf);
}

double SbfAudioStream::_get_length() const {
	if (_bank.is_null()) {
		return 0.0;
	}
	const SbfRawEntry *e = _bank->raw_entry_at(_entry_index);
	if (!e || e->block_size == 0) {
		return 0.0;
	}
	// Each chunk yields up to SBF_CHUNK_AUDIO int16 values where alternating
	// values are L,R,L,R,... so SBF_CHUNK_AUDIO/2 stereo frames per full
	// chunk. The last chunk may be partial (chunk header valid_samples can be
	// less than SBF_CHUNK_AUDIO). Avoiding a per-chunk header read keeps this
	// O(1): upper-bound the frame count assuming every chunk is full. The
	// AudioStreamPlayer treats _get_length() as a hint for UI scrubbers; the
	// playback stops at the decoded PCM's end.
	const uint32_t chunk_count = e->total_size / e->block_size;
	const uint64_t frames = (uint64_t)chunk_count * (SBF_CHUNK_AUDIO / 2);
	return (double)frames / (double)SBF_SAMPLE_RATE;
}
