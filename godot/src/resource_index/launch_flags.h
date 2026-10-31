#pragma once

#include <base/resource_index/boot_policy.h>

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <optional>
#include <string>
#include <vector>

namespace godot {

// The original-engine launch flags the runtime honors and the boot resource
// directory ladder they feed (engine base/resource_index/boot_policy.h).
// This class only samples the process command line and the filesystem for the
// engine's policy; every rule lives engine-side.
class LaunchFlags : public RefCounted {
	GDCLASS(LaunchFlags, RefCounted)

public:
	// True when `/d` (dev / loose-override) was passed.
	static bool loose_override_enabled();
	// `/exp <name>` (or `/mod`, the last one), else `fallback` ("" the base game).
	static String expansion(const String &fallback);
	// The title the launch runs as (a title code): `/game`'s, else `fallback`'s, an empty
	// fallback meaning the launch product's (boot_policy.h launch_game, LaunchProduct::game).
	static String game(const String &fallback);
	// The distribution product: `/game` when given, else the launch directory's
	// UPDATE.WIZ (boot_policy.h folder_product), a folder with no wizard OpenNova's;
	// "" for a folder whose wizard names another game (boot_policy.h launch_product).
	static String product();
	static bool product_supported();
	// The product's command file (JOPTR.CMD, a player game's <KEY>.CMD); a foreign
	// folder's, the CMD its wizard names.
	static String product_command_file();
	// The product's name on the runtime's screens: a stock game's, a player game's
	// own (its installed command file's PRODUCT, else its login); "" for none.
	static String product_name();
	// The product's installed release: its command file's [HEADER] VERSION in the launch
	// directory (boot_policy.h launch_product_version, the file the updater's version mark
	// reads), "0.1.4"; "" when the file is absent or names none. Read once per launch.
	static String product_version();
	// The launch's versions on one line (base/version/version_line.h): the product's name and
	// its installed release, ONE's build (one_build()) and the protocol its title speaks
	// (net/protocol/protocol.h title_versions), "OP: Acid
	// Gambit 0.1.4 · ONE 0.0.22 (a1b2c3d) · JO protocol 16"; a part unknown left out.
	static String version_line();
	// ONE's version, the build this program is (base/version/one_build.h one_build()): "0.0.22",
	// a staging run's "0.0.22.123". The game MCP endpoint's serverInfo names it.
	static String one_version();
	// True when `/game` was passed, even without a value.
	static bool game_given();
	static bool no_update();
	// True when update.exe relaunched the game after a first-run content install.
	static bool after_update();
	// The directory the launch reads: `--resource-dir`, else the executable's.
	static String launch_directory();
	// `--resource-dir <path>` (process-local, never persisted), or "".
	static String resource_dir();
	// True when `--resource-dir` was passed, even without a value.
	static bool resource_dir_given();
	// `--loose-mission <name.bms>`, or "" for the normal menu flow.
	static String loose_mission();
	// True when `--loose-root` was passed (ADR 0025).
	static bool loose_root_allowed();

	// The runtime launch vocabulary (boot_policy.h): "" / the fallback / the
	// sentinel when the flag is absent or malformed.
	static String mission();
	static String lan_host();
	// `--lan-rotation`: the --lan-host rotation's missions after its first.
	static PackedStringArray lan_rotation();
	static String lan_join_ip();
	static int lan_join_port(int fallback);
	// `--lan-join browse[:port]`: search, then join the row on the port.
	static bool lan_join_browse();
	static int lan_port(int fallback);
	// -1 = auto (derive the game type from the mission).
	static int lan_gametype();
	static int lan_mode(int fallback);
	static int lan_max_players(int fallback);
	static bool spectator();
	static String spectator_password();
	static String callsign();
	static String capture_pcap();
	// 0 = no runtime MCP endpoint.
	static int mcp_port();
	// `--novaworld-host <host>`: the OpenNova target's gate; "" = the product's own.
	static String novaworld_host();
	// `--novaworld-join <rid>`: the NovaWorld server to join once listed; 0 = none.
	static int novaworld_join();
	// `/NOHUD` — the HUD overlay master word's clear (boot_policy.h no_hud).
	static bool no_hud();
	// `/noreload` — the auto-reload global forced off (boot_policy.h no_reload).
	static bool no_reload();
	// `/LAN`: every game.cfg read forces mpnovaworldhostlanonly (boot_policy.h lan).
	static bool lan();
	// `/NOPUNT`, retail's no-punt switch, as parsed (boot_policy.h nopunt).
	static bool nopunt();

	// The directory the game was started in, '/'-separated, where retail keeps the files it
	// writes beside itself, its saves among them (weapon.sav: PlayerProfile_LoadAllFromDisk @
	// 0x54f4d0 builds the path relative to it), and which the editor's Play makes its run
	// directory (ADR 0046 S13 A8), so the build the game runs from is never written:
	// `--working-dir <path>`, which a source run passes since Godot's `--path` moved the process
	// to the project, else the process's working directory (a packaged runtime's own).
	static String working_dir();
	// `/sstga`: the Screen Shot action writes a run-length TGA instead of the JPEG (boot_policy.h
	// screenshot_tga).
	static bool screen_shot_tga();
	// The file the Screen Shot action writes: the first SS%0.5d.jpg (.tga under /sstga) the
	// working directory does not hold, under it; "" when every index is taken (engine
	// gameprofile/player_files.h screenshot_file_name carries the witness).
	static String screen_shot_path();
	// The /sstga arm's file: the frame (`rgba`, width x height RGBA8, the top row first) as the
	// run-length TGA (engine formats/tga/tga.h tga_write_screenshot carries the witness).
	static PackedByteArray screen_shot_tga_file(const PackedByteArray &rgba, int width, int height);

	// The updater's command lines (boot_policy.h updater_check_arguments /
	// updater_install_arguments): the check's, and the install's with the relaunch's
	// arguments (`run_args`, less an earlier launch's markers) and this one's.
	static PackedStringArray updater_check_arguments(const String &product,
			const String &runtime_dir, const String &resource_dir);
	static PackedStringArray updater_install_arguments(const String &product,
			const String &runtime_dir, const String &resource_dir, int64_t parent_pid,
			const PackedStringArray &run_args, bool bootstrap, int64_t join_rid);

	// The whole parsed set, for a C++ consumer that reads several switches at once (the
	// NovaWorld connect's command line, NovaWorldClient::start).
	static opennova::LaunchFlags parsed() { return parse(); }

	// Tests substitute the launch token list; the real runtime parses its own
	// command line (engine + user args).
	static void set_args_override(const PackedStringArray &args);
	static void clear_args_override();
	static bool has_args_override();

protected:
	static void _bind_methods();

private:
	static opennova::LaunchFlags parse();

	// A std::string, not a godot::String: a file-scope godot::String would be
	// constructed at DLL load, before godot-cpp's runtime is bound.
	static std::vector<std::string> args_override_;
	static bool args_override_set_;
	// product_version's read: the command file it read (its folder and name) and its VERSION.
	static std::string read_version_file_;
	static std::optional<opennova::Version> read_version_;
	static bool version_read_;
	static std::optional<opennova::Version> product_version_of(const opennova::LaunchFlags &flags);
};

} // namespace godot
