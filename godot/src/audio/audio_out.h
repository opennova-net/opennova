#pragma once

#include <godot_cpp/classes/audio_stream_generator_playback.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>

#include <cstdint>
#include <vector>

#include <runtime/audio/mixer.h>
#include <runtime/audio/sound_options.h>

namespace godot {

// The audio device (ADR 0077 d4): the process's one engine mixer (runtime/audio/mixer.h) and its
// sound option words, and the one stream Godot plays them through, an AudioStreamGenerator at the
// mixer's 44100 Hz on the Master bus, whose gain is the only level Godot keeps. Each frame the
// generator's free frames are mixed and pushed, the stream kept a fifth of a second ahead as
// retail's fill keeps it (audio::kMixFillFrames). The device node lives under the tree's root,
// attached the first time a sound wants it.
class AudioOut : public Node {
	GDCLASS(AudioOut, Node)

public:
	// The volume byte's ceiling and the cfg's default channel volume (runtime/audio/sound_options.h).
	enum {
		VOLUME_BYTE_MAX = opennova::audio::kVolumeByteMax,
		DEFAULT_CHANNEL_VOLUME = opennova::audio::kDefaultChannelVolume,
	};

	static opennova::audio::Mixer &mixer();
	static opennova::audio::SoundOptions &options();
	// The device node under `p_any`'s tree root, made once (deferred into the tree).
	static void attach(Node *p_any);
	// The options' three words (the cfg's SFXLevel, DialogueLevel and music_volume).
	static void set_option_volumes(int p_sound_fx, int p_dialogue, int p_music);
	// The three words as the sounds read them (sound_fx, dialogue, music; the tests' read).
	static PackedInt32Array option_volumes();
	// The device frames mixed since the process began (the tests' measure).
	static int64_t frames_mixed();
	// Mixes `p_frames` frames the device does not play (the tests' clock: a wave ends when the
	// mixer has rendered it out).
	static void mix_frames(int p_frames);

	void _ready() override;
	void _process(double p_delta) override;

protected:
	static void _bind_methods();

private:
	void pump();

	Ref<AudioStreamGeneratorPlayback> playback_;
	std::vector<int16_t> pcm_;
};

} // namespace godot
