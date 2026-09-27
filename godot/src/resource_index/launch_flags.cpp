#include "resource_index/launch_flags.h"
#include "util/string_convert.h"

#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include <string>
#include <vector>

namespace godot {

using opennova::to_gd;
using opennova::to_std;

std::vector<std::string> LaunchFlags::args_override_;
bool LaunchFlags::args_override_set_ = false;

void LaunchFlags::_bind_methods() {
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("loose_override_enabled"),
			&LaunchFlags::loose_override_enabled);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("mission"), &LaunchFlags::mission);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("lan_host"), &LaunchFlags::lan_host);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("lan_join_ip"), &LaunchFlags::lan_join_ip);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("lan_join_port", "fallback"),
			&LaunchFlags::lan_join_port);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("lan_port", "fallback"),
			&LaunchFlags::lan_port);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("lan_gametype"), &LaunchFlags::lan_gametype);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("lan_mode", "fallback"),
			&LaunchFlags::lan_mode);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("lan_max_players", "fallback"),
			&LaunchFlags::lan_max_players);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("spectator"), &LaunchFlags::spectator);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("spectator_password"),
			&LaunchFlags::spectator_password);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("callsign"), &LaunchFlags::callsign);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("integrity_profile"),
			&LaunchFlags::integrity_profile);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("capture_pcap"), &LaunchFlags::capture_pcap);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("mcp_port"), &LaunchFlags::mcp_port);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("set_args_override", "args"),
			&LaunchFlags::set_args_override);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("clear_args_override"),
			&LaunchFlags::clear_args_override);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("has_args_override"),
			&LaunchFlags::has_args_override);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("expansion", "fallback"),
			&LaunchFlags::expansion, DEFVAL(String()));
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("game", "fallback"),
			&LaunchFlags::game, DEFVAL(String("jo")));
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("resource_dir"),
			&LaunchFlags::resource_dir);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("resource_dir_given"),
			&LaunchFlags::resource_dir_given);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("loose_mission"),
			&LaunchFlags::loose_mission);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("loose_root_allowed"),
			&LaunchFlags::loose_root_allowed);
}

// Every token the game was launched with (engine + user args): Godot commands
// put custom options behind Godot's `--` separator, and scanning both arrays
// lets packaged and source launches share the engine's one parser. Tests
// substitute the token list through set_args_override.
opennova::LaunchFlags LaunchFlags::parse() {
	if (args_override_set_) return opennova::parse_launch_flags(args_override_);
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

bool LaunchFlags::loose_override_enabled() {
	return parse().loose_override;
}

String LaunchFlags::expansion(const String &fallback) {
	return to_gd(opennova::launch_expansion(parse(), to_std(fallback)));
}

String LaunchFlags::game(const String &fallback) {
	return to_gd(opennova::launch_game(parse(), to_std(fallback)));
}

String LaunchFlags::resource_dir() {
	return to_gd(parse().resource_dir);
}

bool LaunchFlags::resource_dir_given() {
	return parse().resource_dir_given;
}

String LaunchFlags::loose_mission() {
	return to_gd(parse().loose_mission);
}

bool LaunchFlags::loose_root_allowed() {
	return parse().loose_root;
}

String LaunchFlags::mission() {
	return to_gd(parse().mission);
}

String LaunchFlags::lan_host() {
	return to_gd(parse().lan_host);
}

String LaunchFlags::lan_join_ip() {
	return to_gd(opennova::launch_lan_join_endpoint(parse(), 0).ip);
}

int LaunchFlags::lan_join_port(int fallback) {
	return opennova::launch_lan_join_endpoint(parse(), fallback).port;
}

int LaunchFlags::lan_port(int fallback) {
	return opennova::launch_lan_port(parse(), fallback);
}

int LaunchFlags::lan_gametype() {
	return parse().lan_gametype;
}

int LaunchFlags::lan_mode(int fallback) {
	return opennova::launch_lan_mode(parse(), fallback);
}

int LaunchFlags::lan_max_players(int fallback) {
	return opennova::launch_lan_max_players(parse(), fallback);
}

bool LaunchFlags::spectator() {
	return parse().spectator;
}

String LaunchFlags::spectator_password() {
	return to_gd(parse().spectator_password);
}

String LaunchFlags::callsign() {
	return to_gd(parse().callsign);
}

String LaunchFlags::integrity_profile() {
	return to_gd(parse().integrity_profile);
}

String LaunchFlags::capture_pcap() {
	return to_gd(parse().capture_pcap);
}

int LaunchFlags::mcp_port() {
	return parse().mcp_port;
}

void LaunchFlags::set_args_override(const PackedStringArray &args) {
	args_override_.clear();
	args_override_.reserve(static_cast<size_t>(args.size()));
	for (int64_t i = 0; i < args.size(); ++i) args_override_.push_back(to_std(args[i]));
	args_override_set_ = true;
}

void LaunchFlags::clear_args_override() {
	args_override_.clear();
	args_override_set_ = false;
}

bool LaunchFlags::has_args_override() {
	return args_override_set_;
}

} // namespace godot
