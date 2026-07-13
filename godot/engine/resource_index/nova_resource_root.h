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

#include <cstdint>
#include <string>
#include <unordered_map>

#include <resource_index/resource_index.h>

namespace godot {

class NovaResourceRoot : public RefCounted {
	GDCLASS(NovaResourceRoot, RefCounted)

	String root_dir_;
	String last_error_;
	opennova::ResourceIndex index_;

	// resolve_file memo: lowercased flat name -> on-disk path (empty = case-variant
	// duplicates, an error per the resolve contract). Built from ONE directory walk
	// per cache epoch; resolve_file used to walk the whole root per call, and mission
	// loads resolve thousands of texture names against multi-thousand-file roots.
	// Snapshot-at-epoch matches the index_-backed listings: on-disk edits surface via
	// scan/mount or bump_cache_epoch(), exactly as documented above cache_epoch().
	mutable std::unordered_map<std::string, String> resolve_memo_;
	mutable uint64_t resolve_memo_epoch_ = 0;
	mutable bool resolve_memo_built_ = false;

	// Decoded-texture cache for the packed (PFF) fallback in load_texture. The loose
	// path already memoizes inside util/texture_path_resolver; PFF-resident textures
	// used to re-extract + re-decode on every call. Negative results cache too — the
	// material resolvers probe load_texture for names resolve_file can't see, and a
	// miss costs the full candidate scan. Same epoch self-clear as the memo above.
	mutable std::unordered_map<std::string, Ref<Texture2D>> packed_texture_cache_;
	mutable uint64_t packed_texture_cache_epoch_ = 0;

	static bool has_virtual_scheme(const String &path);
	static String to_native_path(const String &path);
	static String normalize_dir(const String &path);
	static String lookup_name(const String &name);
	static Dictionary file_entry_to_dictionary(const opennova::ResourceFileEntry &entry);

	// Shared validate-and-scan body for every mount entry point. `discovery` splits packed
	// runtime mounting from authoring discovery; SCR decoding is always version-detected.
	Error mount_with_mode(const String &path, const String &expansion, opennova::VfsMountMode mode,
	                      opennova::VfsArchiveDiscovery discovery);

	String expansion_;

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
	// otherwise the runtime reads from PFFs exclusively.
	Error mount_runtime(const String &path, const String &expansion = String(),
	                    bool allow_loose_override = false);
	// ONED Play mount: loose files only, PFF archives ignored. Unlike set_root_dir(), this
	// accepts an expansion so a directory-only authoring expansion can layer over the base root.
	Error mount_loose_runtime(const String &path, const String &expansion = String());
	// Global cache epoch (see util/engine_caches.h): bumped by every mount/clear on ANY
	// root. GDScript cache holders compare it against the epoch they were built under and
	// self-clear when it moved. bump_cache_epoch() lets the editor force-invalidate after
	// editing files on disk without remounting.
	static int64_t cache_epoch();
	static void bump_cache_epoch();
	// The expansion name this root was runtime-mounted with ("" for base game or an
	// editor browse mount). Feeds the expansion bank slots and the M<exp>/G<exp>
	// music forms [orig: Expansion_LoadAssets @ 0x4a4730].
	String get_expansion() const;

	// Expansion names discoverable under `<path>/expansion/` (each subdir with a matching
	// <name>.pff). Independent of the currently mounted root, so the UI can list before mounting.
	PackedStringArray list_expansions(const String &path) const;
	// Every immediate expansion subdirectory, including directory-only authoring expansions.
	// ONED sessions use this seam; retail discovery remains list_expansions().
	PackedStringArray list_loose_expansions(const String &path) const;
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

	// The witnessed boot-required manifest (ENG-6, libs/gameprofile
	// required_resources; witness source docs/required-resources.md).
	// list_missing_boot_resources probes the FATAL-class file rows against
	// this mounted root and returns the missing names (empty = boot-viable).
	// The boot-archive-table trio is excluded: its all-missing gate is
	// enforced by mount_runtime itself [orig: PFF_OpenAllArchives @ 0x4a4310;
	// fatal check @ 0x4a6f44]. boot_resource_failure_text quotes the
	// witnessed failure behavior for a manifest name ("" for unknown names)
	// so hosts can raise honest missing-resource errors.
	PackedStringArray list_missing_boot_resources() const;
	String boot_resource_failure_text(const String &name) const;

	// C++ siblings only (not bound): direct read access to the underlying index
	// so NovaReferenceIndex can list entries with their size/mtime stamps
	// without Variant-boxing the whole listing through GDScript dictionaries.
	const opennova::ResourceIndex &native_index() const { return index_; }
};

} // namespace godot
