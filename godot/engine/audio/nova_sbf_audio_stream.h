#ifndef NOVA_SBF_AUDIO_STREAM_H
#define NOVA_SBF_AUDIO_STREAM_H

#include <godot_cpp/classes/audio_stream.hpp>
#include <godot_cpp/classes/audio_stream_playback.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

#include "nova_sbf_bank.h"

namespace godot {

// AudioStream wrapper for one entry inside a NovaSbfBank. Holds a strong Ref to
// the bank (the entry data it reads at playback time lives inside the bank, and
// the playback runs on the audio thread, so a raw back-pointer would dangle if
// the bank were freed mid-mix) plus the entry index. Playback is delegated to
// NovaSbfAudioStreamPlayback, which opens its own FileAccess so multiple
// players can stream independently from the same bank.
class NovaSbfAudioStream : public AudioStream {
	GDCLASS(NovaSbfAudioStream, AudioStream)

public:
	void configure(NovaSbfBank *p_bank, int p_entry_index);

	NovaSbfBank *get_bank() const { return _bank.ptr(); }
	int get_entry_index() const { return _entry_index; }
	int get_index() const { return _entry_index; }

	// AudioStream overrides
	Ref<AudioStreamPlayback> _instantiate_playback() const override;
	String _get_stream_name() const override;
	double _get_length() const override;
	bool _is_monophonic() const override { return false; }
	double _get_bpm() const override { return 0.0; }

protected:
	static void _bind_methods();

private:
	Ref<NovaSbfBank> _bank;
	int _entry_index = -1;
};

} // namespace godot

#endif // NOVA_SBF_AUDIO_STREAM_H
