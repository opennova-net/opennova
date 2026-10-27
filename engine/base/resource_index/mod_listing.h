#pragma once

// A game's mods on NovaWorld, as the runtime's NovaWorld menu lists, installs and removes them
// (ADR 0074). The list is the service's GET /api/mods?game=<command file>; a mod installs into its
// expansion folder, expansion/<code>/, whose UPDATE.WIZ alone makes it a product of the updater's
// own (apps/update discover_products, as retail's updater takes every expansion folder holding a
// wizard), so writing the wizard and running the updater installs it.

#include <string>
#include <string_view>
#include <vector>

namespace opennova {

// One mod as the service lists it: its product key, its expansion code (the folder and /exp name),
// its name, its owner's account, its published release's version, and the updater product its
// wizard names (PRODUCT_LOGIN and CMD).
struct ModListing {
	std::string key;
	std::string code;
	std::string name;
	std::string owner;
	std::string version;
	std::string updater_login;
	std::string command_file;
};

// The reply's mods, {"mods": [{key, code, name, owner, version, updater_login, command_file}]}, in
// its order. An entry that cannot install is left out: one with no code expansion_code_valid takes,
// no updater login or no command file. False with `error` for a reply that is no such object.
bool read_mod_listings(std::string_view json, std::vector<ModListing> &out, std::string &error);

// Whether `code` can name a mod's folder: a single folder name (no separator, not "." or "..") the
// game can mount, at most kExpansionNameMax characters (boot_policy.h).
bool expansion_code_valid(const std::string &code);

// Writes <game_root>/expansion/<code>/UPDATE.WIZ naming the mod's updater product, and the update
// host the runtime's OPENNOVA.WIZ in `runtime_dir` names (its URL; none without one, which the
// updater never dials anyway). The folder is made when absent. False with `error` for a code
// expansion_code_valid refuses, a value a wizard cannot carry, or a failed write.
bool write_mod_wizard(const std::string &game_root, const std::string &runtime_dir, const ModListing &mod,
		std::string &error);

// Deletes <game_root>/expansion/<code>/ with everything in it. True when it is gone (or was never
// there); false with `error` for a code expansion_code_valid refuses or a file that would not go
// (one the game holds open).
bool remove_mod_folder(const std::string &game_root, const std::string &code, std::string &error);

} // namespace opennova
