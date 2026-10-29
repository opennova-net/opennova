#include "resource_index/launch_flags.h"
#include "util/string_convert.h"
#include <base/gameprofile/player_files.h>
#include <base/io/cp1252.h>
#include <base/proc/child_process.h>
#include <base/version/one_build.h>
#include <base/version/version_line.h>
#include <net/protocol/protocol.h>
#include <formats/tga/tga.h>

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace godot {

using opennova::to_gd;
using opennova::to_std;

std::vector<std::string> LaunchFlags::args_override_;
bool LaunchFlags::args_override_set_ = false;
std::string LaunchFlags::read_version_file_;
std::optional<opennova::Version> LaunchFlags::read_version_;
bool LaunchFlags::version_read_ = false;

void LaunchFlags::_bind_methods() {
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("loose_override_enabled"),
			&LaunchFlags::loose_override_enabled);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("mission"), &LaunchFlags::mission);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("lan_host"), &LaunchFlags::lan_host);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("lan_rotation"), &LaunchFlags::lan_rotation);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("lan_join_ip"), &LaunchFlags::lan_join_ip);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("lan_join_port", "fallback"),
			&LaunchFlags::lan_join_port);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("lan_join_browse"),
			&LaunchFlags::lan_join_browse);
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
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("capture_pcap"), &LaunchFlags::capture_pcap);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("mcp_port"), &LaunchFlags::mcp_port);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("novaworld_host"), &LaunchFlags::novaworld_host);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("novaworld_join"), &LaunchFlags::novaworld_join);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("no_hud"), &LaunchFlags::no_hud);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("screen_shot_tga"), &LaunchFlags::screen_shot_tga);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("screen_shot_path"), &LaunchFlags::screen_shot_path);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("screen_shot_tga_file", "rgba", "width", "height"),
			&LaunchFlags::screen_shot_tga_file);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("no_reload"), &LaunchFlags::no_reload);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("lan"), &LaunchFlags::lan);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("nopunt"), &LaunchFlags::nopunt);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("set_args_override", "args"),
			&LaunchFlags::set_args_override);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("working_dir"), &LaunchFlags::working_dir);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("clear_args_override"),
			&LaunchFlags::clear_args_override);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("has_args_override"),
			&LaunchFlags::has_args_override);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("expansion", "fallback"),
			&LaunchFlags::expansion, DEFVAL(String()));
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("game", "fallback"),
			&LaunchFlags::game, DEFVAL(String()));
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("product"), &LaunchFlags::product);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("product_supported"), &LaunchFlags::product_supported);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("product_command_file"),
			&LaunchFlags::product_command_file);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("product_name"), &LaunchFlags::product_name);
	ClassDB::bind_static_method(
			"LaunchFlags", D_METHOD("product_version"), &LaunchFlags::product_version);
	ClassDB::bind_static_method(
			"LaunchFlags", D_METHOD("version_line"), &LaunchFlags::version_line);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("game_given"), &LaunchFlags::game_given);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("no_update"), &LaunchFlags::no_update);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("after_update"), &LaunchFlags::after_update);
	ClassDB::bind_static_method("LaunchFlags", D_METHOD("launch_directory"),
			&LaunchFlags::launch_directory);
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
// lets packaged and source launches share the engine's one parser. The retail
// switches are walked over the game's own line, as the game's config tokenizer
// splits it. A packaged game's is the process's line where the OS keeps one
// (Windows: `opennova.exe /exp jox01`, and the updater's relaunch of it,
// `... /exp jox01 -- --after-update`), else its arguments spelled as one. A source
// run (the editor binary, no "template" feature) puts Godot's own arguments ahead of
// `--` (its --path, and the scene's res:// path, whose "//" would end the line), so
// its line is the arguments after `--`, spelled as one. The walk reads the line
// without OpenNova's own options (without_own_options), which retail's never held.
// Tests substitute the token list through set_args_override, which is walked as a
// line spelling it.
opennova::LaunchFlags LaunchFlags::parse() {
	if (args_override_set_) return opennova::parse_launch_flags(args_override_);
	std::vector<std::string> args;
	std::vector<std::string> user;
	bool source_run = false;
	OS *os = OS::get_singleton();
	if (os != nullptr) {
		source_run = !os->has_feature("template");
		const PackedStringArray engine_args = os->get_cmdline_args();
		const PackedStringArray user_args = os->get_cmdline_user_args();
		args.reserve(static_cast<size_t>(engine_args.size() + user_args.size()));
		for (int64_t i = 0; i < engine_args.size(); ++i) args.push_back(to_std(engine_args[i]));
		for (int64_t i = 0; i < user_args.size(); ++i) user.push_back(to_std(user_args[i]));
		args.insert(args.end(), user.begin(), user.end());
	}
	std::string line = source_run ? opennova::retail_command_line(user) : opennova::proc::own_command_line();
	if (line.empty()) line = opennova::retail_command_line(args);
	return opennova::parse_launch_flags(args, line);
}

bool LaunchFlags::loose_override_enabled() {
	return parse().loose_override;
}

String LaunchFlags::expansion(const String &fallback) {
	return to_gd(opennova::launch_expansion(parse(), to_std(fallback)));
}

namespace {
std::string launch_dir_of(const opennova::LaunchFlags &flags) {
	return flags.resource_dir.empty() ? to_std(OS::get_singleton()->get_executable_path().get_base_dir())
									  : flags.resource_dir;
}
opennova::LaunchProduct product_of(const opennova::LaunchFlags &flags) {
	return opennova::launch_product(flags, opennova::folder_product(launch_dir_of(flags)));
}
} // namespace

String LaunchFlags::game(const String &fallback) {
	const opennova::LaunchFlags flags = parse();
	return to_gd(fallback.is_empty() ? product_of(flags).game : opennova::launch_game(flags, to_std(fallback)));
}

String LaunchFlags::product() { return to_gd(product_of(parse()).product); }
bool LaunchFlags::product_supported() { return product_of(parse()).supported; }
String LaunchFlags::product_command_file() { return to_gd(product_of(parse()).command); }
String LaunchFlags::product_name() { return to_gd(product_of(parse()).name); }

// Read once: a launch's command file is the updater's to change, with the game closed. A test's
// args override is a new launch, which reads its own.
std::optional<opennova::Version> LaunchFlags::product_version_of(
		const opennova::LaunchFlags &flags) {
	const std::string dir = launch_dir_of(flags);
	const opennova::LaunchProduct product = product_of(flags);
	const std::string file = opennova::boot_path_join(dir, product.command);
	if (!version_read_ || file != read_version_file_) {
		read_version_ = opennova::launch_product_version(dir, product);
		read_version_file_ = file;
		version_read_ = true;
	}
	return read_version_;
}

String LaunchFlags::product_version() {
	const std::optional<opennova::Version> version = product_version_of(parse());
	return version ? to_gd(version->text()) : String();
}

String LaunchFlags::version_line() {
	const opennova::LaunchFlags flags = parse();
	const opennova::LaunchProduct product = product_of(flags);
	opennova::Versions versions;
	// A player game's name is its command file's PRODUCT, the page's bytes.
	versions.game = opennova::retail_text_to_utf8(product.name);
	versions.game_version = product_version_of(flags);
	versions.one = opennova::one_build();
	// The title the launch runs as: its name where the product has none, the protocol it speaks.
	opennova::title_versions(product.game, versions);
	return to_gd(opennova::version_line(versions));
}

bool LaunchFlags::game_given() { return parse().game_given; }
bool LaunchFlags::no_update() { return parse().no_update; }
bool LaunchFlags::after_update() { return parse().after_update; }

String LaunchFlags::launch_directory() { return to_gd(launch_dir_of(parse())); }

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

PackedStringArray LaunchFlags::lan_rotation() {
	PackedStringArray out;
	for (const std::string &name : parse().lan_rotation) out.push_back(to_gd(name));
	return out;
}

String LaunchFlags::lan_join_ip() {
	return to_gd(opennova::launch_lan_join_endpoint(parse(), 0).ip);
}

int LaunchFlags::lan_join_port(int fallback) {
	return opennova::launch_lan_join_endpoint(parse(), fallback).port;
}

bool LaunchFlags::lan_join_browse() {
	return opennova::launch_lan_join_endpoint(parse(), 0).browse;
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

String LaunchFlags::capture_pcap() {
	return to_gd(parse().capture_pcap);
}

String LaunchFlags::novaworld_host() {
	return to_gd(parse().novaworld_host);
}

int LaunchFlags::novaworld_join() { return parse().novaworld_join; }

int LaunchFlags::mcp_port() {
	return parse().mcp_port;
}

bool LaunchFlags::screen_shot_tga() {
	return parse().screenshot_tga;
}

String LaunchFlags::screen_shot_path() {
	const String dir = working_dir();
	const std::string name = opennova::gameprofile::screenshot_file_name(
			[&dir](const std::string &p_name) { return FileAccess::file_exists(dir.path_join(to_gd(p_name))); },
			screen_shot_tga());
	return name.empty() ? String() : dir.path_join(to_gd(name));
}

PackedByteArray LaunchFlags::screen_shot_tga_file(const PackedByteArray &rgba, int width, int height) {
	PackedByteArray out;
	if (width <= 0 || height <= 0 || rgba.size() < int64_t(width) * height * 4) return out;
	std::vector<uint8_t> file;
	std::string error;
	if (!opennova::tga::tga_write_screenshot(rgba.ptr(), uint32_t(width), uint32_t(height), file, error)) return out;
	out.resize(int64_t(file.size()));
	std::copy(file.begin(), file.end(), out.ptrw());
	return out;
}

bool LaunchFlags::no_hud() {
	return parse().no_hud;
}

bool LaunchFlags::no_reload() {
	return parse().no_reload;
}

bool LaunchFlags::lan() {
	return parse().lan;
}

bool LaunchFlags::nopunt() {
	return parse().nopunt;
}

void LaunchFlags::set_args_override(const PackedStringArray &args) {
	args_override_.clear();
	args_override_.reserve(static_cast<size_t>(args.size()));
	for (int64_t i = 0; i < args.size(); ++i) args_override_.push_back(to_std(args[i]));
	args_override_set_ = true;
	version_read_ = false; // a new launch reads its release again
}

String LaunchFlags::working_dir() {
	// The flag's UTF-8 text as given, '/'-separated (a narrow std::filesystem::path would read it
	// in the ANSI code page).
	std::string given = parse().working_dir;
	std::replace(given.begin(), given.end(), '\\', '/');
	if (!given.empty()) return to_gd(given);
	std::error_code ec;
	const std::filesystem::path cwd = std::filesystem::current_path(ec);
	return ec ? String() : to_gd(cwd.generic_u8string());
}

void LaunchFlags::clear_args_override() {
	args_override_.clear();
	args_override_set_ = false;
	version_read_ = false;
}

bool LaunchFlags::has_args_override() {
	return args_override_set_;
}

} // namespace godot
