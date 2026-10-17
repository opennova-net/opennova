#pragma once

#include <godot_cpp/classes/audio_stream.hpp>
#include <godot_cpp/classes/audio_stream_playback.hpp>
#include <godot_cpp/classes/audio_stream_wav.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

#include "audio/sbf_bank.h"

namespace godot {

// AudioStream wrapper for one entry inside a SbfBank: a strong Ref to the bank
// plus the entry index. Each playback is a built-in one, Godot's
// AudioStreamWAV playback over the entry's PCM as the engine decodes it
// (sbf::sbf_decode_entry), never an extension object: the AudioServer keeps a
// stopped playback in its list until its own teardown, which Godot's
// Main::cleanup runs after it has unregistered every extension class, so an
// extension playback still listed at a forced quit (`--quit-after`, a quit with
// the music streaming) was freed through its unregistered class (signal 11 at
// exit; docs/audio/mus-sbf-re.md "Godot playback shutdown").
class SbfAudioStream : public AudioStream {
	GDCLASS(SbfAudioStream, AudioStream)

public:
	void configure(SbfBank *p_bank, int p_entry_index);

	SbfBank *get_bank() const { return _bank.ptr(); }
	int get_index() const { return _entry_index; }
	// The entry's PCM (sbf::sbf_decode_entry) as a 16-bit stereo AudioStreamWAV
	// at SBF_SAMPLE_RATE: what each playback plays. Null for an entry the bank
	// does not hold.
	Ref<AudioStreamWAV> get_wav() const;

	// AudioStream overrides
	Ref<AudioStreamPlayback> _instantiate_playback() const override;
	String _get_stream_name() const override;
	double _get_length() const override;
	bool _is_monophonic() const override { return false; }
	double _get_bpm() const override { return 0.0; }

protected:
	static void _bind_methods();

private:
	Ref<SbfBank> _bank;
	int _entry_index = -1;
};

} // namespace godot

