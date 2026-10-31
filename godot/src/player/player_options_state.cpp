#include "player/player_options_state.h"

#include <godot_cpp/core/property_info.hpp>

namespace godot {

Ref<PlayerOptionsState> PlayerOptionsState::make(const opennova::inmatch::PlayerOptions &p_value) {
	Ref<PlayerOptionsState> state;
	state.instantiate();
	state->value_ = p_value;
	return state;
}

PackedInt32Array PlayerOptionsState::to_words() const {
	const opennova::inmatch::PlayerOptionsWords words =
			opennova::inmatch::player_options_words(value_);
	PackedInt32Array out;
	out.resize(static_cast<int64_t>(words.size()));
	for (size_t i = 0; i < words.size(); ++i) out.set(static_cast<int64_t>(i), words[i]);
	return out;
}

void PlayerOptionsState::set_words(const PackedInt32Array &p_words) {
	value_ = opennova::inmatch::player_options_from_words(value_, p_words.ptr(),
			static_cast<size_t>(p_words.size()));
}

#define PLAYER_OPTIONS_PROPERTY(p_type, p_name)                                                     \
	ClassDB::bind_method(D_METHOD("get_" #p_name), &PlayerOptionsState::get_##p_name);             \
	ClassDB::bind_method(D_METHOD("set_" #p_name, "value"), &PlayerOptionsState::set_##p_name);    \
	ADD_PROPERTY(PropertyInfo(Variant::p_type, #p_name), "set_" #p_name, "get_" #p_name)

void PlayerOptionsState::_bind_methods() {
	ClassDB::bind_method(D_METHOD("copy"), &PlayerOptionsState::copy);
	ClassDB::bind_method(D_METHOD("to_words"), &PlayerOptionsState::to_words);
	ClassDB::bind_method(D_METHOD("set_words", "words"), &PlayerOptionsState::set_words);
	PLAYER_OPTIONS_PROPERTY(INT, sound_fx_volume);
	PLAYER_OPTIONS_PROPERTY(INT, dialog_volume);
	PLAYER_OPTIONS_PROPERTY(INT, music_volume);
	PLAYER_OPTIONS_PROPERTY(INT, crosshair_style);
	PLAYER_OPTIONS_PROPERTY(INT, crosshair_color);
	PLAYER_OPTIONS_PROPERTY(BOOL, crosshair_spread);
	PLAYER_OPTIONS_PROPERTY(INT, aspect_mode);
	PLAYER_OPTIONS_PROPERTY(BOOL, keyboard_tips);
	PLAYER_OPTIONS_PROPERTY(BOOL, gameplay_tips);
	PLAYER_OPTIONS_PROPERTY(INT, object_polydetail);
	PLAYER_OPTIONS_PROPERTY(INT, texfilter_level);
	PLAYER_OPTIONS_PROPERTY(INT, particle_density);
	PLAYER_OPTIONS_PROPERTY(INT, texcompression_level);
	PLAYER_OPTIONS_PROPERTY(INT, terrain_texdetail);
	PLAYER_OPTIONS_PROPERTY(INT, object_texdetail);
}

#undef PLAYER_OPTIONS_PROPERTY

} // namespace godot
