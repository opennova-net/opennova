#include "resource_index/launch_flags.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include <string>
#include <vector>

namespace godot {

String LaunchFlags::bundled_probe_override_;

void LaunchFlags::_bind_methods() {
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("loose_override_enabled"),
			&LaunchFlags::loose_override_enabled);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("expansion", "fallback"),
			&LaunchFlags::expansion, DEFVAL(String()));
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("game", "fallback"),
			&LaunchFlags::game, DEFVAL(String("jo")));
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("resource_dir", "fallback"),
			&LaunchFlags::resource_dir, DEFVAL(String()));
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("loose_mission"),
			&LaunchFlags::loose_mission);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("loose_root_allowed"),
			&LaunchFlags::loose_root_allowed);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("boot_resource_dir", "persisted"),
			&LaunchFlags::boot_resource_dir);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("boot_loose_allowed", "dir"),
			&LaunchFlags::boot_loose_allowed);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("bundled_game_dir", "exe_dir"),
			&LaunchFlags::bundled_game_dir);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("bundled_assets_dir", "exe_dir"),
			&LaunchFlags::bundled_assets_dir);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("set_bundled_probe_override", "dir"),
			&LaunchFlags::set_bundled_probe_override);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("get_bundled_probe_override"),
			&LaunchFlags::get_bundled_probe_override);
}

std::string LaunchFlags::to_std(const String &s) {
	return std::string(s.utf8().get_data());
}

String LaunchFlags::from_std(const std::string &s) {
	return String::utf8(s.c_str());
}

// Every token the game was launched with (engine + user args): Godot commands
// put custom options behind Godot's `--` separator, and scanning both arrays
// lets packaged and source launches share the engine's one parser.
opennova::LaunchFlags LaunchFlags::parse() {
	std::vector<std::string> args;
	OS *os = OS::get_singleton();
	if (os != nullptr) {
		const PackedStringArray engine_args = os->get_cmdline_args();
		const PackedStringArray user_args = os->get_cmdline_user_args();
		args.reserve(static_cast<size_t>(engine_args.size() + user_args.size()));
		for (int64_t i = 0; i < engine_args.size(); ++i) args.push_back(to_std(engine_args[i]));
		for (int64_t i = 0; i < user_args.size(); ++i) args.push_back(to_std(user_args[i]));
	}
	return opennova::parse_launch_flags(args);
}

opennova::BootDirProbe LaunchFlags::probe() {
	opennova::BootDirProbe p;
	p.file_exists = [](const std::string &path) {
		return FileAccess::file_exists(from_std(path));
	};
	p.dir_exists = [](const std::string &path) {
		return DirAccess::dir_exists_absolute(from_std(path));
	};
	return p;
}

std::string LaunchFlags::probe_dir() {
	if (!bundled_probe_override_.is_empty()) return to_std(bundled_probe_override_);
	OS *os = OS::get_singleton();
	return os != nullptr ? to_std(os->get_executable_path().get_base_dir()) : std::string();
}

bool LaunchFlags::loose_override_enabled() {
	return parse().loose_override;
}

String LaunchFlags::expansion(const String &fallback) {
	return from_std(opennova::launch_expansion(parse(), to_std(fallback)));
}

String LaunchFlags::game(const String &fallback) {
	return from_std(opennova::launch_game(parse(), to_std(fallback)));
}

String LaunchFlags::resource_dir(const String &fallback) {
	return from_std(opennova::launch_resource_dir(parse(), to_std(fallback)));
}

String LaunchFlags::loose_mission() {
	return from_std(parse().loose_mission);
}

bool LaunchFlags::loose_root_allowed() {
	return parse().loose_root;
}

String LaunchFlags::boot_resource_dir(const String &persisted) {
	return from_std(opennova::boot_resource_dir(parse(), to_std(persisted), probe_dir(), probe()));
}

bool LaunchFlags::boot_loose_allowed(const String &dir) {
	return opennova::boot_loose_allowed(parse(), to_std(dir), probe_dir(), probe());
}

String LaunchFlags::bundled_game_dir(const String &exe_dir) {
	return from_std(opennova::bundled_game_dir(to_std(exe_dir), probe()));
}

String LaunchFlags::bundled_assets_dir(const String &exe_dir) {
	return from_std(opennova::bundled_assets_dir(to_std(exe_dir), probe()));
}

void LaunchFlags::set_bundled_probe_override(const String &dir) {
	bundled_probe_override_ = dir;
}

String LaunchFlags::get_bundled_probe_override() {
	return bundled_probe_override_;
}

} // namespace godot
