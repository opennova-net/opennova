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

public:
	enum LookupPolicy {
		LOOKUP_SESSION_DEFAULT = 0,
		LOOKUP_FORCE_LOOSE_FIRST,
		LOOKUP_FORCE_ARCHIVE_ONLY,
	};

private:
	enum class MountKind {
		None,
		EditorLoose,
		Runtime,
	};

	String root_dir_;
	String last_error_;
	opennova::ResourceIndex index_;
	MountKind mount_kind_ = MountKind::None;

	// resolve_file memo: lowercased flat name -> on-disk path (empty = case-variant
	// duplicates, an error per the resolve contract). Built from ONE directory walk
	// per cache epoch; resolve_file used to walk the whole root per call, and mission
	// loads resolve thousands of texture names against multi-thousand-file roots.
	// Snapshot-at-epoch matches the index_-backed listings: on-disk edits surface via
	// scan/mount or bump_cache_epoch(), exactly as documented above cache_epoch().
	mutable std::unordered_map<std::string, String> resolve_memo_;
	mutable uint64_t resolve_memo_epoch_ = 0;
	mutable bool resolve_memo_built_ = false;

	// Decoded-texture cache for VFS-backed load_texture. Both archive and loose winners
	// resolve through the mounted index so the mount mode owns precedence. Negative
	// results cache too — material resolvers probe load_texture for names resolve_file
	// can't see, and a miss costs the full candidate scan. Same epoch self-clear as the
	// memo above.
	mutable std::unordered_map<std::string, Ref<Texture2D>> texture_cache_;
	mutable uint64_t texture_cache_epoch_ = 0;

	static bool has_virtual_scheme(const String &path);
	static String to_native_path(const String &path);
	static String normalize_dir(const String &path);
	static String lookup_name(const String &name);
	static Dictionary file_entry_to_dictionary(const opennova::ResourceFileEntry &entry);
	static opennova::VfsLookupPolicy to_vfs_lookup_policy(LookupPolicy policy);

	// Shared validate-and-scan body for both mount entry points. `game_code` selects the SCR
	// decode policy (gameprofile code, e.g. "jo"/"jodemo"); an empty/unknown code is the JO default.
	// `discovery` splits the two products: the runtime mounts the witnessed retail boot table,
	// the editor's browse index scans every archive (D-VFS-2's recorded decision).
	Error mount_with_mode(const String &path, const String &expansion, opennova::VfsMountMode mode,
	                      const String &game_code, opennova::VfsArchiveDiscovery discovery);

	String expansion_;

protected:
	static void _bind_methods();

public:
	static bool is_valid_root(const String &path);

	// Editor / authoring mount: loose files only, PFF archives ignored. This is the path the
	// editor and the test fixtures use, so authoring always targets loose files.
	Error set_root_dir(const String &path);
	// Runtime mount: the PFF archives are the packed game data. At least one fixed-table archive
	// must open; a loose-only directory is not a viable install, including under `/d`. `expansion`
	// (e.g. "jox01") layers the expansion's archives over the base game. `/d` makes loose-first
	// the session default; without it, retained loose roots are visible only to explicit retail
	// force-loose-first calls. `game_code` (the `/game <code>` launch flag, default "jo") selects
	// the SCR decode key so demo data decodes correctly.
	// Safe to call on an already-mounted root: the mount is replaced, not layered (the index
	// rebuilds, the resolver caches drop, and the epoch bump self-clears every epoch-keyed
	// holder), so holders of this object move with it — the in-place expansion switch the
	// Mods screen and the LAN joiner both perform.
	Error mount_runtime(const String &path, const String &expansion = String(),
	                    bool allow_loose_override = false, const String &game_code = "jo");
	// Global cache epoch (see util/engine_caches.h): bumped by every mount/clear on ANY
	// root. GDScript cache holders compare it against the epoch they were built under and
	// self-clear when it moved. bump_cache_epoch() lets the editor force-invalidate after
	// editing files on disk without remounting.
	static int64_t cache_epoch();
	static void bump_cache_epoch();
	// The expansion this root ACTUALLY mounted ("" for base game, editor/loose mounts, and
	// after mount_runtime's silent base fallback for an expansion that is not installed).
	// Feeds the expansion bank slots and the M<exp>/G<exp> music forms
	// [orig: Expansion_LoadAssets @ 0x4a4730]. Never reports the requested name back: a
	// caller that must not run on the wrong data set (the LAN joiner reconciling against the
	// host's authoritative expansion, D-NET-178) needs this to be evidence, not an echo.
	String get_expansion() const;
	// True only while a successful mount_runtime() mount is live: this root's data is the
	// packed game install. Editor mounts (set_root_dir), never-mounted roots, clear(), and
	// every failed mount report false. A caller re-mounting a root it did not create (the LAN
	// joiner switching an injected runtime root to the host's expansion, D-NET-178) reads this
	// to pick the right entry point: re-mounting a runtime root through set_root_dir would
	// silently drop its archives and leave the session on loose files.
	bool is_runtime_mount() const;

	// Expansion names discoverable under `<path>/expansion/` (each subdir with a matching
	// <name>.pff). Independent of the currently mounted root, so the UI can list before mounting.
	PackedStringArray list_expansions(const String &path) const;
	String get_root_dir() const;
	String get_last_error() const;
	void clear();

	String resolve_file(const String &name);
	PackedStringArray list_files(const String &suffix = String()) const;
	Array list_file_entries(const String &suffix = String()) const;
	// Runtime roots honor the retail per-call source policy. Editor roots deliberately
	// retain their legacy loose-only flat lookup for every policy value.
	bool has_file(const String &name, LookupPolicy policy = LOOKUP_SESSION_DEFAULT) const;
	PackedByteArray read_file(const String &name, LookupPolicy policy = LOOKUP_SESSION_DEFAULT) const;
	Ref<Texture2D> load_texture(const String &name, LookupPolicy policy = LOOKUP_SESSION_DEFAULT) const;
	Ref<Resource> load_font(const String &name) const;

	// The witnessed boot-required manifest (ENG-6, libs/gameprofile
	// required_resources; witness source docs/required-resources.md).
	// list_missing_boot_resources probes the FATAL-class file rows against
	// this mounted root and returns the missing names (empty = boot-viable).
	// The boot-archive-table trio is excluded: its all-missing gate is
	// enforced by mount_runtime itself [orig: PFF_OpenAllArchives @ 0x4a4310;
	// fatal check @ 0x4a6f44]. boot_resource_failure_text quotes the
	// witnessed failure behavior for a manifest name ("" for unknown names)
	// so owners can raise honest missing-resource errors.
	PackedStringArray list_missing_boot_resources() const;
	String boot_resource_failure_text(const String &name) const;

	// C++ siblings only (not bound): direct read access to the underlying index
	// so NovaReferenceIndex can list entries with their size/mtime stamps
	// without Variant-boxing the whole listing through GDScript dictionaries.
	const opennova::ResourceIndex &native_index() const { return index_; }
};

} // namespace godot

VARIANT_ENUM_CAST(godot::NovaResourceRoot::LookupPolicy);
