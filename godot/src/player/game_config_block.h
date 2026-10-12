#pragma once

// The process's game.cfg block binding: one engine inmatch::GameConfigBlock
// (the cfg block, its file in the game's working directory, the weapon table
// its avail_wpn rows address), the mount a main-menu entry reads over, and the
// shell's words by their game.cfg keys. Every rule is the engine's
// (runtime/inmatch/game_config_block.h, docs/gamecfg/game-cfg-re.md); this
// class marshals.

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <runtime/inmatch/game_config_block.h>

namespace godot {

class ResourceRoot;
class RtxtStringFile;

class GameConfigBlock : public RefCounted {
	GDCLASS(GameConfigBlock, RefCounted)

	opennova::inmatch::GameConfigBlock block_;

protected:
	static void _bind_methods();

public:
	opennova::inmatch::GameConfigBlock &native() { return block_; }
	const opennova::inmatch::GameConfigBlock &native() const { return block_; }

	// The file every read opens and the save writes (game.cfg in the working
	// directory, an OS path).
	void set_path(const String &p_path);
	String get_path() const;

	// The start's read: false when the file set mpreset, where the process
	// exits with code 0 and writes nothing.
	bool load() { return block_.load(); }
	// The save: OK, or ERR_FILE_CANT_WRITE (logged) when the file does not open.
	Error save();
	// A main-menu entry over the mounted `p_root` (its weapon.def table, the
	// `p_gametext` default texts, `p_desktop` for the first launch's video
	// test), and the expansion or mod reload: false when a read set mpreset.
	bool enter_main_menu(const Ref<ResourceRoot> &p_root, const Ref<RtxtStringFile> &p_gametext,
			const Vector2i &p_desktop);
	bool reload_main_menu(const Ref<ResourceRoot> &p_root, const Ref<RtxtStringFile> &p_gametext,
			const Vector2i &p_desktop);

	bool is_file_read() const { return block_.file_read(); }
	bool is_version_reset() const { return block_.version_reset(); }
	bool is_reset_exit() const { return block_.reset_exit(); }
	bool is_video_test_ran() const { return block_.video_test_ran(); }

	// The shell's int words by their game.cfg keys (gamecfg::int_word): an
	// unknown key reads 0 and writes nothing (a warning).
	int get_word(const String &p_key) const;
	void set_word(const String &p_key, int p_value);
};

} // namespace godot
