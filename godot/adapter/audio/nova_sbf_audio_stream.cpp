// AudioStream wrapper for a single entry inside a NovaSbfBank.
// Witnessed engine equivalent: jointops!Sbf_StartEntry @ 0x004ED910 sets up
// per-entry streaming state (block_size / total_size / data_offset) which
// NovaSbfAudioStreamPlayback consumes via raw_entry_at() at playback time.

#include "nova_sbf_audio_stream.h"

#include "nova_sbf_audio_stream_playback.h"
#include "nova_sbf_bank.h"

#include <godot_cpp/core/class_db.hpp>

#include <cstring>

using namespace godot;

void NovaSbfAudioStream::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_index"), &NovaSbfAudioStream::get_index);
	ClassDB::bind_method(D_METHOD("get_entry_index"), &NovaSbfAudioStream::get_entry_index);
}

void NovaSbfAudioStream::configure(NovaSbfBank *p_bank, int p_entry_index) {
	_bank = Ref<NovaSbfBank>(p_bank);
	_entry_index = p_entry_index;
}

Ref<AudioStreamPlayback> NovaSbfAudioStream::_instantiate_playback() const {
	Ref<NovaSbfAudioStreamPlayback> pb;
	pb.instantiate();
	pb->bind_to(_bank, _entry_index);
	return pb;
}

String NovaSbfAudioStream::_get_stream_name() const {
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

double NovaSbfAudioStream::_get_length() const {
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
	// playback path stops cleanly at the real end-of-stream when _load_chunk
	// hits past total_size.
	const uint32_t chunk_count = e->total_size / e->block_size;
	const uint64_t frames = (uint64_t)chunk_count * (SBF_CHUNK_AUDIO / 2);
	return (double)frames / (double)SBF_SAMPLE_RATE;
}
