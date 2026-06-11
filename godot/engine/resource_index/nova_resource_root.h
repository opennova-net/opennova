#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <resource_index/resource_index.h>

namespace godot {

class NovaResourceRoot : public RefCounted {
	GDCLASS(NovaResourceRoot, RefCounted)

	String root_dir_;
	String last_error_;
	opennova::ResourceIndex index_;

	static bool has_virtual_scheme(const String &path);
	static String to_native_path(const String &path);
	static String normalize_dir(const String &path);
	static String lookup_name(const String &name);
	static Dictionary file_entry_to_dictionary(const opennova::ResourceFileEntry &entry);

	// Shared validate-and-scan body for both mount entry points. `game_code` selects the SCR
	// decode policy (gameprofile code, e.g. "jo"/"jodemo"); an empty/unknown code is the JO default.
	Error mount_with_mode(const String &path, const String &expansion, opennova::VfsMountMode mode,
	                      const String &game_code);

protected:
	static void _bind_methods();

public:
	static bool is_valid_root(const String &path);

	// Editor / authoring mount: loose files only, PFF archives ignored. This is the path the
	// editor and the test fixtures use, so authoring always targets loose files.
	Error set_root_dir(const String &path);
	// Runtime mount: the PFF archives are the packed game data. `expansion` (e.g. "jox01")
	// layers the expansion's archives over the base game. When `allow_loose_override` is true
	// (the engine's `/d` dev flag) loose files next to the archives shadow the packed entries;
	// otherwise the runtime reads from PFFs exclusively. `game_code` (the `/game <code>` launch
	// flag, default "jo") selects the SCR decode key so demo data decodes correctly.
	Error mount_runtime(const String &path, const String &expansion = String(),
	                    bool allow_loose_override = false, const String &game_code = "jo");
	// Global cache epoch (see util/engine_caches.h): bumped by every mount/clear on ANY
	// root. GDScript cache holders compare it against the epoch they were built under and
	// self-clear when it moved. bump_cache_epoch() lets the editor force-invalidate after
	// editing files on disk without remounting.
	static int64_t cache_epoch();
	static void bump_cache_epoch();
	// Expansion names discoverable under `<path>/expansion/` (each subdir with a matching
	// <name>.pff). Independent of the currently mounted root, so the UI can list before mounting.
	PackedStringArray list_expansions(const String &path) const;
	String get_root_dir() const;
	String get_last_error() const;
	void clear();

	String resolve_file(const String &name);
	PackedStringArray list_files(const String &suffix = String()) const;
	Array list_file_entries(const String &suffix = String()) const;
	bool has_file(const String &name) const;
	PackedByteArray read_file(const String &name) const;
	Ref<Texture2D> load_texture(const String &name) const;
	Ref<Resource> load_font(const String &name) const;

	// C++ siblings only (not bound): direct read access to the underlying index
	// so NovaReferenceIndex can list entries with their size/mtime stamps
	// without Variant-boxing the whole listing through GDScript dictionaries.
	const opennova::ResourceIndex &native_index() const { return index_; }
};

} // namespace godot
