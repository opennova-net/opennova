#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <runtime/audio/volume_law.h>

namespace godot {

// The volume-law statics the options screen reads, reachable from GDScript. The runtime sound
// bank (audio/sound_bank) drives the engine's opennova::audio::SoundSelector directly through
// the oneshot_play plan, so no selection state lives here.
class SoundSelector : public RefCounted {
	GDCLASS(SoundSelector, RefCounted)

protected:
	static void _bind_methods();

public:
	// The engine's channel volume law (engine/runtime/audio/volume_law.h): the
	// 0..255 byte ceiling and the byte -> dB conversion (0 = hard silent).
	enum { VOLUME_BYTE_MAX = opennova::audio::kVolumeByteMax,
        DEFAULT_CHANNEL_VOLUME = opennova::audio::kDefaultChannelVolume };
	static double volume_db_from_255(int volume);
};

} // namespace godot
