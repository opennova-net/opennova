#include "audio/sound_selector.h"

using namespace godot;

double SoundSelector::volume_db_from_255(int volume) {
	return opennova::audio::volume_db_from_byte(volume);
}

void SoundSelector::_bind_methods() {
	BIND_CONSTANT(VOLUME_BYTE_MAX);
    BIND_CONSTANT(DEFAULT_CHANNEL_VOLUME);
	ClassDB::bind_static_method("SoundSelector", D_METHOD("volume_db_from_255", "volume"),
			&SoundSelector::volume_db_from_255);
}
