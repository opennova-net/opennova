#include "resource_index/mod_listing.h"

#include "util/record_bind.h"
#include "util/string_convert.h"

#include <godot_cpp/classes/ref.hpp>

#include <string>
#include <vector>

namespace godot {

using opennova::to_gd;
using opennova::to_std;

void ModListing::_bind_methods() {
	ClassDB::bind_static_method("ModListing", D_METHOD("read_reply", "json", "listings"), &ModListing::read_reply);
	ClassDB::bind_static_method("ModListing", D_METHOD("remove_folder", "game_root", "code"),
			&ModListing::remove_folder);
	ClassDB::bind_static_method("ModListing", D_METHOD("folder_marked", "game_root", "code"),
			&ModListing::folder_marked);
	ClassDB::bind_static_method("ModListing", D_METHOD("marked_folders", "game_root"),
			&ModListing::marked_folders);
	ClassDB::bind_method(D_METHOD("write_wizard", "game_root", "runtime_dir"), &ModListing::write_wizard);
	OPENNOVA_RECORD_READ_ONLY(ModListing, Variant::STRING, key)
	OPENNOVA_RECORD_READ_ONLY(ModListing, Variant::STRING, code)
	OPENNOVA_RECORD_READ_ONLY(ModListing, Variant::STRING, name)
	OPENNOVA_RECORD_READ_ONLY(ModListing, Variant::STRING, owner)
	OPENNOVA_RECORD_READ_ONLY(ModListing, Variant::STRING, version)
	OPENNOVA_RECORD_READ_ONLY(ModListing, Variant::STRING, updater_login)
	OPENNOVA_RECORD_READ_ONLY(ModListing, Variant::STRING, command_file)
}

String ModListing::read_reply(const String &json, TypedArray<ModListing> listings) {
	std::vector<opennova::ModListing> mods;
	std::string error;
	if (!opennova::read_mod_listings(to_std(json), mods, error)) return to_gd(error);
	for (const opennova::ModListing &mod : mods) {
		Ref<ModListing> row;
		row.instantiate();
		row->value_ = mod;
		listings.push_back(row);
	}
	return String();
}

String ModListing::remove_folder(const String &game_root, const String &code) {
	std::string error;
	return opennova::remove_mod_folder(to_std(game_root), to_std(code), error) ? String() : to_gd(error);
}

bool ModListing::folder_marked(const String &game_root, const String &code) {
	return opennova::mod_folder_marked(to_std(game_root), to_std(code));
}

PackedStringArray ModListing::marked_folders(const String &game_root) {
	PackedStringArray out;
	for (const std::string &code : opennova::marked_mod_folders(to_std(game_root))) out.push_back(to_gd(code));
	return out;
}

String ModListing::write_wizard(const String &game_root, const String &runtime_dir) const {
	std::string error;
	return opennova::write_mod_wizard(to_std(game_root), to_std(runtime_dir), value_, error) ? String()
	                                                                                       : to_gd(error);
}

String ModListing::get_key() const { return to_gd(value_.key); }
String ModListing::get_code() const { return to_gd(value_.code); }
String ModListing::get_name() const { return to_gd(value_.name); }
String ModListing::get_owner() const { return to_gd(value_.owner); }
String ModListing::get_version() const { return to_gd(value_.version); }
String ModListing::get_updater_login() const { return to_gd(value_.updater_login); }
String ModListing::get_command_file() const { return to_gd(value_.command_file); }

} // namespace godot
