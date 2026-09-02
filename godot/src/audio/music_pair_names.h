#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

#include <runtime/audio/music_policy.h>

namespace godot {

// One witnessed interactive-music pair by NAME (runtime/audio/music_policy.h
// MusicPairNames): the stem, the loose .sbf bank file, the VFS .bin script
// file and the expansion subdir ("" for the base pair). Pure name resolution;
// MusicService keeps the on-disk probing. Read-only, assigned by
// MusicDirector::resolve_menu_music_pair / resolve_game_music_pair.
class MusicPairNames : public RefCounted {
	GDCLASS(MusicPairNames, RefCounted)

public:
	void assign(const opennova::audio::MusicPairNames &p_names);

	String get_stem() const { return stem_; }
	String get_bank_file() const { return bank_file_; }
	String get_script_file() const { return script_file_; }
	String get_subdir() const { return subdir_; }

protected:
	static void _bind_methods();

private:
	String stem_;
	String bank_file_;
	String script_file_;
	String subdir_;
};

} // namespace godot
