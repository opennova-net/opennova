#include "audio/music_pair.h"
#include "util/string_convert.h"

using namespace godot;

void MusicPair::assign(const opennova::audio::MusicPair &p_pair) {
	bank_ = opennova::to_gd(p_pair.bank);
	script_name_ = opennova::to_gd(p_pair.script_name);
}

void MusicPair::_bind_methods() {
#define MUSIC_PAIR_READ_ONLY(m_name)                                                             \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &MusicPair::get_##m_name);                   \
	ADD_PROPERTY(PropertyInfo(Variant::STRING, #m_name, PROPERTY_HINT_NONE, "",                 \
						 PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY),                    \
			"", "get_" #m_name);
	MUSIC_PAIR_READ_ONLY(bank)
	MUSIC_PAIR_READ_ONLY(script_name)
#undef MUSIC_PAIR_READ_ONLY
}
