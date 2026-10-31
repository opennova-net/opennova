#pragma once

// The player's options record (engine runtime/inmatch/player_options.h) as the shell's scripts
// read and edit it: one detached copy per read, handed back to the cfg block's write
// (GameConfigBlock::write_player_options). The words' mapping, the clamps and the defaults are
// the engine's; this class marshals the fields.

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>

#include <runtime/inmatch/player_options.h>

namespace godot {

class PlayerOptionsState : public RefCounted {
	GDCLASS(PlayerOptionsState, RefCounted)

	opennova::inmatch::PlayerOptions value_ = opennova::inmatch::player_options_defaults();

protected:
	static void _bind_methods();

public:
	static Ref<PlayerOptionsState> make(const opennova::inmatch::PlayerOptions &p_value);
	const opennova::inmatch::PlayerOptions &value() const { return value_; }

	Ref<PlayerOptionsState> copy() const { return make(value_); }
	// The words the Options screens show, by MenuDriver.OPTIONS_WORD_*, and the record back from
	// their edit (a short array leaves it as it stands).
	PackedInt32Array to_words() const;
	void set_words(const PackedInt32Array &p_words);

	int get_sound_fx_volume() const { return value_.sound_fx_volume; }
	void set_sound_fx_volume(int p_value) { value_.sound_fx_volume = p_value; }
	int get_dialog_volume() const { return value_.dialog_volume; }
	void set_dialog_volume(int p_value) { value_.dialog_volume = p_value; }
	int get_music_volume() const { return value_.music_volume; }
	void set_music_volume(int p_value) { value_.music_volume = p_value; }
	int get_crosshair_style() const { return value_.crosshair_style; }
	void set_crosshair_style(int p_value) { value_.crosshair_style = p_value; }
	int get_crosshair_color() const { return value_.crosshair_color; }
	void set_crosshair_color(int p_value) { value_.crosshair_color = p_value; }
	bool get_crosshair_spread() const { return value_.crosshair_spread; }
	void set_crosshair_spread(bool p_value) { value_.crosshair_spread = p_value; }
	int get_aspect_mode() const { return value_.aspect_mode; }
	void set_aspect_mode(int p_value) { value_.aspect_mode = p_value; }
	bool get_keyboard_tips() const { return value_.keyboard_tips; }
	void set_keyboard_tips(bool p_value) { value_.keyboard_tips = p_value; }
	bool get_gameplay_tips() const { return value_.gameplay_tips; }
	void set_gameplay_tips(bool p_value) { value_.gameplay_tips = p_value; }
	int get_object_polydetail() const { return value_.object_polydetail; }
	void set_object_polydetail(int p_value) { value_.object_polydetail = p_value; }
	int get_texfilter_level() const { return value_.texfilter_level; }
	void set_texfilter_level(int p_value) { value_.texfilter_level = p_value; }
	int get_particle_density() const { return value_.particle_density; }
	void set_particle_density(int p_value) { value_.particle_density = p_value; }
	int get_texcompression_level() const { return value_.texcompression_level; }
	void set_texcompression_level(int p_value) { value_.texcompression_level = p_value; }
	int get_terrain_texdetail() const { return value_.terrain_texdetail; }
	void set_terrain_texdetail(int p_value) { value_.terrain_texdetail = p_value; }
	int get_object_texdetail() const { return value_.object_texdetail; }
	void set_object_texdetail(int p_value) { value_.object_texdetail = p_value; }
};

} // namespace godot
