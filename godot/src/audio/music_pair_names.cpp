#include "audio/music_pair_names.h"
#include "util/string_convert.h"

using namespace godot;

void MusicPairNames::assign(const opennova::audio::MusicPairNames &p_names) {
	stem_ = opennova::to_gd(p_names.stem);
	bank_file_ = opennova::to_gd(p_names.bank_file);
	script_file_ = opennova::to_gd(p_names.script_file);
	subdir_ = opennova::to_gd(p_names.subdir);
}

void MusicPairNames::_bind_methods() {
#define MUSIC_PAIR_NAMES_READ_ONLY(m_name)                                                       \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &MusicPairNames::get_##m_name);              \
	ADD_PROPERTY(PropertyInfo(Variant::STRING, #m_name, PROPERTY_HINT_NONE, "",                 \
						 PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY),                    \
			"", "get_" #m_name);
	MUSIC_PAIR_NAMES_READ_ONLY(stem)
	MUSIC_PAIR_NAMES_READ_ONLY(bank_file)
	MUSIC_PAIR_NAMES_READ_ONLY(script_file)
	MUSIC_PAIR_NAMES_READ_ONLY(subdir)
#undef MUSIC_PAIR_NAMES_READ_ONLY
}
