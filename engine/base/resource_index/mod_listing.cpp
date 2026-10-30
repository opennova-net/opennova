// A game's mods on NovaWorld and their folders -- see mod_listing.h. OpenNova's own (ADR 0074):
// no retail counterpart to cite.

#include <base/resource_index/mod_listing.h>

#include <base/io/file_io.h>
#include <base/io/json.h>
#include <base/io/os_path.h>
#include <base/io/strutil.h>
#include <base/resource_index/boot_policy.h>
#include <base/vfs/vfs.h>
#include <formats/update/apply.h>
#include <formats/update/update_wizard.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <system_error>

namespace opennova {

namespace fs = std::filesystem;

namespace {

std::string refused_code(const std::string &code) { return "\"" + code + "\" is no mod folder's name."; }

} // namespace

bool read_mod_listings(std::string_view json, std::vector<ModListing> &out, std::string &error) {
	out.clear();
	io::JsonValue reply;
	if (!io::json_parse(json, reply, error)) return false;
	const io::JsonValue *mods = reply.get("mods");
	if (!mods || !mods->is_array()) {
		error = "The reply lists no mods.";
		return false;
	}
	for (const io::JsonValue &entry : mods->array) {
		ModListing mod;
		mod.key = entry.get_string("key", "");
		mod.code = strutil::trim(entry.get_string("code", ""));
		mod.name = entry.get_string("name", "");
		mod.owner = entry.get_string("owner", "");
		mod.version = entry.get_string("version", "");
		mod.updater_login = strutil::trim(entry.get_string("updater_login", ""));
		mod.command_file = strutil::trim(entry.get_string("command_file", ""));
		if (expansion_code_valid(mod.code) && !mod.updater_login.empty() && !mod.command_file.empty())
			out.push_back(std::move(mod));
	}
	return true;
}

bool expansion_code_valid(const std::string &code) {
	return update::is_safe_row_name(code) && code.size() <= kExpansionNameMax;
}

bool write_mod_wizard(const std::string &game_root, const std::string &runtime_dir, const ModListing &mod,
		std::string &error) {
	if (!expansion_code_valid(mod.code)) {
		error = refused_code(mod.code);
		return false;
	}
	// The update host the runtime's own wizard names (OPENNOVA.WIZ, found without case).
	update::UpdateWizard wizard;
	std::vector<uint8_t> runtime;
	if (vfs_game_root_file(runtime_dir, "OPENNOVA.WIZ", runtime))
		wizard.url = update::read_update_wizard(std::string(runtime.begin(), runtime.end())).url;
	wizard.cmd = mod.command_file;
	wizard.product_login = mod.updater_login;
	const std::string text = update::write_update_wizard(wizard);
	if (text.empty() || wizard.cmd.empty() || wizard.product_login.empty()) {
		error = "The mod's product cannot be written into an UPDATE.WIZ.";
		return false;
	}
	const std::string dir = vfs_expansion_dir(game_root, mod.code);
	std::error_code ec;
	fs::create_directories(io::os_path(dir), ec);
	if (ec) {
		error = "Cannot make " + dir + ": " + ec.message();
		return false;
	}
	return io::write_file_atomic(boot_path_join(dir, "UPDATE.WIZ"), text, error) &&
			io::write_file_atomic(boot_path_join(dir, kModMarkFileName), mod.key + "\n", error);
}

bool mod_folder_marked(const std::string &game_root, const std::string &code) {
	if (!expansion_code_valid(code)) return false;
	std::error_code ec;
	return fs::is_regular_file(io::os_path(boot_path_join(vfs_expansion_dir(game_root, code), kModMarkFileName)), ec);
}

std::vector<std::string> marked_mod_folders(const std::string &game_root) {
	std::vector<std::string> codes;
	std::error_code ec;
	for (const fs::directory_entry &entry :
			fs::directory_iterator(io::os_path(boot_path_join(game_root, "expansion")), ec)) {
		std::error_code kind;
		const std::string code = io::utf8_path(entry.path().filename());
		if (entry.is_directory(kind) && mod_folder_marked(game_root, code)) codes.push_back(code);
	}
	std::sort(codes.begin(), codes.end());
	return codes;
}

bool remove_mod_folder(const std::string &game_root, const std::string &code, std::string &error) {
	if (!expansion_code_valid(code)) {
		error = refused_code(code);
		return false;
	}
	const std::string dir = vfs_expansion_dir(game_root, code);
	std::error_code ec;
	if (fs::exists(io::os_path(dir), ec) && !mod_folder_marked(game_root, code)) {
		error = "\"" + code + "\" is no mod the Mods page installed: its folder stays.";
		return false;
	}
	fs::remove_all(io::os_path(dir), ec);
	if (ec) {
		error = "Cannot remove " + dir + ": " + ec.message();
		return false;
	}
	return true;
}

} // namespace opennova
