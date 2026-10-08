#pragma once

#include <base/resource_index/boot_policy.h>

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

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
	// `/game <code>` lowercased, else the persisted fallback, else "jo".
	static String game(const String &fallback);
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
	static String lan_join_ip();
	static int lan_join_port(int fallback);
	static int lan_port(int fallback);
	// -1 = auto (derive the game type from the mission).
	static int lan_gametype();
	static int lan_mode(int fallback);
	static int lan_max_players(int fallback);
	static bool spectator();
	static String spectator_password();
	static String callsign();
	static String integrity_profile();
	static String capture_pcap();
	// 0 = no runtime MCP endpoint.
	static int mcp_port();
	// `/NOHUD` — the HUD overlay master word's clear (boot_policy.h no_hud).
	static bool no_hud();
	// `/noreload` — the auto-reload global forced off (boot_policy.h no_reload).
	static bool no_reload();

	// The directory the game was started in, '/'-separated, where retail keeps the files it
	// writes beside itself, its saves among them (weapon.sav: PlayerProfile_LoadAllFromDisk @
	// 0x54f4d0 builds the path relative to it), and which the editor's Play makes its run
	// directory (ADR 0046 S13 A8), so the build the game runs from is never written:
	// `--working-dir <path>`, which a source run passes since Godot's `--path` moved the process
	// to the project, else the process's working directory (a packaged runtime's own).
	static String working_dir();

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
};

} // namespace godot
