#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <runtime/audio/sound_selector.h>
#include <runtime/audio/volume_law.h>

namespace godot {

// Thin Godot binding over opennova::audio::SoundSelector: the portable sound-set member-selection
// state machine (FIRST/RANDOM/SEQUENTIAL/RANDOM_SEQ). The runtime sound bank (audio/sound_bank)
// drives the engine selector directly through the oneshot_play plan; this binding keeps the
// volume-law statics the options screen reads reachable from GDScript.
class SoundSelector : public RefCounted {
	GDCLASS(SoundSelector, RefCounted)

	opennova::audio::SoundSelector selector_;

protected:
	static void _bind_methods();

public:
	// Drop all per-layer selection state (e.g. when reloading banks).
	void reset();

	// The engine's channel volume law (engine/runtime/audio/volume_law.h): the
	// 0..255 byte ceiling and the byte -> dB conversion (0 = hard silent).
	enum { VOLUME_BYTE_MAX = opennova::audio::kVolumeByteMax,
        DEFAULT_CHANNEL_VOLUME = opennova::audio::kDefaultChannelVolume };
	static double volume_db_from_255(int volume);
};

} // namespace godot
