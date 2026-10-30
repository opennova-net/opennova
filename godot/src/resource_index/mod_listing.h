#pragma once

#include <base/resource_index/mod_listing.h>

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

namespace godot {

// One of a game's mods on NovaWorld (engine base/resource_index/mod_listing.h, ADR 0074): a typed
// row of the service's GET /api/mods reply, which writes its own install wizard. The NovaWorld
// menu lists, installs and removes mods through it; every rule lives engine-side.
class ModListing : public RefCounted {
	GDCLASS(ModListing, RefCounted)

	opennova::ModListing value_;

protected:
	static void _bind_methods();

public:
	// The reply's installable mods appended to `listings`; "" when the reply read, else why not.
	static String read_reply(const String &json, TypedArray<ModListing> listings);
	// expansion/<code>/ under `game_root` deleted with everything in it, when the Mods page installed
	// it; "" when it is gone.
	static String remove_folder(const String &game_root, const String &code);
	// Whether expansion/<code>/ is a folder the Mods page installed (its mark), and the codes of every
	// such folder.
	static bool folder_marked(const String &game_root, const String &code);
	// A mod's folder under `game_root`: expansion/<code>/ (engine base/vfs/vfs.h vfs_expansion_dir).
	static String folder(const String &game_root, const String &code);
	static PackedStringArray marked_folders(const String &game_root);
	// expansion/<code>/UPDATE.WIZ naming this mod's product and the runtime's update host, and the
	// page's mark; "" when written.
	String write_wizard(const String &game_root, const String &runtime_dir) const;

	String get_key() const;
	String get_code() const;
	String get_name() const;
	String get_owner() const;
	String get_version() const;
	String get_updater_login() const;
	String get_command_file() const;
};

} // namespace godot
