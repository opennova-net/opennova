#pragma once

#include <base/resource_index/boot_policy.h>

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

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
	// `/exp <name>`, else the persisted fallback.
	static String expansion(const String &fallback);
	// `/game <code>` lowercased, else the persisted fallback, else "jo".
	static String game(const String &fallback);
	// `--resource-dir <path>` (process-local, never persisted), else the fallback.
	static String resource_dir(const String &fallback);
	// `--loose-mission <name.bms>`, or "" for the normal menu flow.
	static String loose_mission();
	// True when `--loose-root` was passed (ADR 0025).
	static bool loose_root_allowed();

	// The boot resource dir: flag > persisted pick > the game bundled around a
	// shipped exe > the loose assets/ beside it. "" means ask.
	static String boot_resource_dir(const String &persisted);
	// Whether `dir` may fall back to a loose mount: --loose-root, or the
	// bundled assets/ default itself.
	static bool boot_loose_allowed(const String &dir);
	// `exe_dir` when it holds any boot-table archive, else "".
	static String bundled_game_dir(const String &exe_dir);
	// `<exe_dir>/assets` when it exists, else "".
	static String bundled_assets_dir(const String &exe_dir);

	// Tests substitute the directory probed for a bundled game; the real
	// runtime probes its own exe's directory.
	static void set_bundled_probe_override(const String &dir);
	static String get_bundled_probe_override();

protected:
	static void _bind_methods();

private:
	static opennova::LaunchFlags parse();
	static opennova::BootDirProbe probe();
	static std::string probe_dir();
	static std::string to_std(const String &s);
	static String from_std(const std::string &s);

	static String bundled_probe_override_;
};

} // namespace godot
