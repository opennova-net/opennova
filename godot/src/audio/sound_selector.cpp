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
	ClassDB::bind_method(
			D_METHOD("select_member", "bank", "set_index", "layer_index", "member_count", "mode"),
			&SoundSelector::select_member);
	ClassDB::bind_method(D_METHOD("reset"), &SoundSelector::reset);
}

int SoundSelector::select_member(int bank, int set_index, int layer_index, int member_count, int mode) {
	const uint64_t key = opennova::audio::SoundSelector::make_key(bank, set_index, layer_index);
	return selector_.select(key, member_count, mode);
}

void SoundSelector::reset() {
	selector_.reset();
}
