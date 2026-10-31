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

class JoinTarget;
class MenuDriver;
class PlayerOptionsState;
class ResourceRoot;
class RtxtStringFile;

class GameConfigBlock : public RefCounted {
	GDCLASS(GameConfigBlock, RefCounted)

	opennova::inmatch::GameConfigBlock block_;

protected:
	static void _bind_methods();

public:
	// The block runs as the game the process was launched as (ResourceRoot::session_title): its
	// game.cfg table and its host screen's caps, ahead of the start's read.
	GameConfigBlock();

	opennova::inmatch::GameConfigBlock &native() { return block_; }
	const opennova::inmatch::GameConfigBlock &native() const { return block_; }

	// The file every read opens and the save writes (game.cfg in the working
	// directory, an OS path).
	void set_path(const String &p_path);
	String get_path() const;

	// The start's read: false when the file set mpreset, where the process
	// exits with code 0 and writes nothing.
	bool load();
	// The save: OK, or ERR_FILE_CANT_WRITE (logged) when the file does not open.
	Error save();
	// A main-menu entry over the mounted `p_root` (its weapon.def table, the
	// `p_gametext` default texts, `p_desktop` for the first launch's video
	// test), and the expansion or mod reload: false when a read set mpreset.
	bool enter_main_menu(const Ref<ResourceRoot> &p_root, const Ref<RtxtStringFile> &p_gametext,
			const Vector2i &p_desktop);
	bool reload_main_menu(const Ref<ResourceRoot> &p_root, const Ref<RtxtStringFile> &p_gametext,
			const Vector2i &p_desktop);
	// The primary desktop the entry's `p_desktop` takes, as retail's video test samples it (the
	// DPI-unaware logical size on Windows: network/novaworld_identity.h primary_desktop_size).
	static Vector2i primary_desktop_size();
	// A join's switch of `p_root` to the host's expansion: its weapon.def table
	// for the reads and saves after, reading and saving nothing (the engine's
	// GameConfigBlock::switch_expansion).
	void switch_expansion(const Ref<ResourceRoot> &p_root);

	bool is_file_read() const { return block_.file_read(); }
	bool is_version_reset() const { return block_.version_reset(); }
	bool is_reset_exit() const { return block_.reset_exit(); }
	bool is_video_test_ran() const { return block_.video_test_ran(); }

	// The shell's int words by their game.cfg keys (gamecfg::int_word): an
	// unknown key reads 0 and writes nothing (a warning).
	int get_word(const String &p_key) const;
	void set_word(const String &p_key, int p_value);

	// The player's options over the block's words (engine runtime/inmatch/player_options.h):
	// the normalized read, and the write of a record (normalized) that returns what it wrote.
	Ref<PlayerOptionsState> read_player_options() const;
	Ref<PlayerOptionsState> write_player_options(const Ref<PlayerOptionsState> &p_state);

	// The host screen over the block (the engine's host_settings.h): the
	// populate's write (the block's cap clamped to the screen's), each
	// control's shown value by its name, and START's read of one control into
	// the block.
	void populate_host_dialog();
	String host_dialog_value(const String &p_control) const;
	bool apply_host_dialog_control(const String &p_control, const String &p_value);
	// The launch's title's host screen (Title::host_screen: JO's MULTI_PLAYER_HOST, DFX2's
	// NW_MULTI_PLAYER), the mp.mnu screen the shell opens for a host and the populate below
	// fills; "" for a title that names none.
	String host_screen() const;
	// The whole screen over the block (the engine's populate_host_screen /
	// read_host_screen): the controls of the title's host screen (host_screen) shown
	// from the block, and START's read of them into it.
	void populate_host_screen(const Ref<MenuDriver> &p_driver);
	void read_host_screen(const Ref<MenuDriver> &p_driver);
	// Single player's mission start's block write after its save (the engine's
	// GameConfigBlock::single_player_mission_start).
	void single_player_mission_start() { block_.single_player_mission_start(); }
	// A join's JSR and JSPP words over the process's block: a target that answered
	// the join screen's choice commits it (the engine's
	// GameConfigBlock::commit_join_choice), and the join sends what the block
	// holds, which the next main-menu entry's read zeroes. With no block (a world
	// the shell gave none) the join sends the target's own choice.
	static void join_choice(const Ref<GameConfigBlock> &p_block, const Ref<JoinTarget> &p_target,
			int &r_role, String &r_spectator_password);
};

} // namespace godot
