#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <unordered_map>
#include <vector>

namespace godot {

class NovaResourceRoot;

// Thin GDExtension wrapper over libs/def items.def parsing (def_parse_items).
// Resolves a mission entity's item id -> its visual model (.3di basename) and a
// few type fields. This is the minimal "database" slice needed to place objects;
// weapon/ammo/hud definitions are gameplay and intentionally not surfaced here.
class NovaItemDatabase : public RefCounted {
	GDCLASS(NovaItemDatabase, RefCounted)

private:
	struct Item {
		int id = 0;
		int type = 0;
		String display_name;
		String graphic;
		String anim_def;
		String sound_profile;
		// items.def *_function class-tag directives. The net layer maps these to a
		// §5.10b wire dispatch class (NovaNetClient::class_from_tag); the placer/
		// renderer doesn't use them. Stored raw so the object DB stays net-agnostic.
		String ai_function;
		String move_function;
		// items.def soundloop_1..7 — the looping ambient sound-set names for a
		// "snd:"-prefixed `type marker` item (e.g. soundloop_1 LPNV_LIGHT). Time-of-day
		// slots; the runtime plays the first non-empty one resolvable in the .lwf bank.
		String soundloops[7];
	};
	std::unordered_map<int, Item> items;
	String source_path;
	String last_error;

	// Items in a stable display order (by display name, then id), since the backing
	// store is unordered. Shared by get_item_ids() / get_items().
	std::vector<const Item *> sorted_items() const;

protected:
	static void _bind_methods();

public:
	// DefItemDef.type values (see libs/def/include/def/def.h).
	enum {
		TYPE_UNKNOWN = 0,
		TYPE_MARKER = 1,
		TYPE_VEHICLE = 2,
		TYPE_PERSON = 3,
		TYPE_BUILDING = 4,
		TYPE_DECORATION = 5,
		TYPE_FOLIAGE = 6,
		TYPE_OBJECT = 7,
		TYPE_POWERUP = 8,
	};

	Error load(const String &path);
	// Load items.def by flat name through the mounted resource root (VFS), so the item
	// database resolves from PFF archives at runtime. Mirrors the other *_from_resource_root.
	Error load_from_resource_root(const Ref<NovaResourceRoot> &p_resource_root, const String &p_name);
	bool is_loaded() const;
	String get_source_path() const;
	String get_last_error() const;
	int get_count() const;

	bool has_item(int id) const;
	// Model basename without extension (e.g. "tank"); empty if unknown/none.
	String get_graphic(int id) const;
	String get_anim_def(int id) const;
	// items.def *_function class tags (raw); empty if the item declares none. The
	// net layer turns these into a wire dispatch class.
	String get_ai_function(int id) const;
	String get_move_function(int id) const;
	int get_item_type(int id) const;
	String get_display_name(int id) const;
	// items.def sound_profile (a sound-set name resolved against the loaded .lwf
	// banks at runtime); empty if the item declares none.
	String get_sound_profile(int id) const;
	// items.def soundloop_1..7 as a 7-entry array (empty strings for unused slots).
	// These are the looping ambient sound-set names for "snd:" marker items.
	PackedStringArray get_sound_loops(int id) const;
	Dictionary get_item(int id) const;

	// Enumeration for UI (e.g. the mission editor's place-object palette). Both are
	// sorted deterministically by (display_name, id) so the list is stable across
	// loads (the backing store is an unordered_map). get_items() returns the same
	// per-item dictionaries as get_item(); get_item_ids() is just the ids.
	PackedInt32Array get_item_ids() const;
	Array get_items() const;
};

} // namespace godot
