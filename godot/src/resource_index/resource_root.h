#pragma once

#include <godot_cpp/classes/image.hpp>
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

#include <base/resource_index/resource_index.h>
#include <runtime/assets/asset_store.h>
#include <runtime/renderer/texture_load_rules.h>

#include <vector>

namespace godot {

class ResourceRoot : public RefCounted {
	GDCLASS(ResourceRoot, RefCounted)

public:
	enum LookupPolicy {
		LOOKUP_SESSION_DEFAULT = 0,
		LOOKUP_FORCE_LOOSE_FIRST,
		LOOKUP_FORCE_ARCHIVE_ONLY,
	};

	// The game's texture loaders, by the role that calls them (the engine's
	// renderer::TextureLoader, same order; renderer/texture_load_rules.h says
	// which file each opens and how it decodes it).
	enum TextureLoader {
		TEXTURE_LOADER_STAGE = 0,
		TEXTURE_LOADER_PLAIN,
		TEXTURE_LOADER_ARCHIVE,
		TEXTURE_LOADER_ARCHIVE_SELF_ALPHA,
		TEXTURE_LOADER_FILE,
		TEXTURE_LOADER_TGA,
		TEXTURE_LOADER_PCX,
		TEXTURE_LOADER_HUD_COLOR,
		TEXTURE_LOADER_HUD_ALPHA,
		TEXTURE_LOADER_MENU,
		TEXTURE_LOADER_CINE_FADE,
		TEXTURE_LOADER_PARTICLE,
	};

private:
	enum class MountKind {
		None,
		Loose,
		Runtime,
	};

	String root_dir_;
	String last_error_;
	opennova::ResourceIndex index_;
	opennova::assets::AssetStore assets_{&index_};
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
	// results cache too, so a missing name is probed once. Same epoch self-clear as the
	// memo above.
	mutable std::unordered_map<std::string, Ref<Texture2D>> texture_cache_;
	mutable uint64_t texture_cache_epoch_ = 0;

public:
	// C++-side seam (not bound): engine consumers fed by this mounted session
	// (the mission catalog builder). The index dies with this ResourceRoot.
	const opennova::ResourceIndex &engine_index() const { return index_; }
	// C++-side seam (not bound): the `/game` code the live mount was made for ("jo" for
	// a loose mount, empty when nothing is mounted); the menu's version label reads it.
	const String &game_code() const { return game_code_; }

private:
	static bool has_virtual_scheme(const String &path);
	static String to_native_path(const String &path);
	static String normalize_dir(const String &path);
	static String lookup_name(const String &name);
	static opennova::VfsLookupPolicy to_vfs_lookup_policy(LookupPolicy policy);

	// The body both mount entry points share before their scan: the caches keyed to the old
	// root dropped, the old mount discarded, `path` validated; `r_clean` the directory to
	// scan, root_dir_ set to it.
	Error begin_mount(const String &path, String &r_clean);
	// The files a texture loader tries for `name` under `policy` (the mount's own
	// query rules, the policy's loose-first answer), and one attempt's bytes.
	std::vector<opennova::renderer::TextureLoad> texture_attempts_(const String &name,
			TextureLoader loader, LookupPolicy policy) const;
	PackedByteArray read_texture_attempt_(const opennova::renderer::TextureLoad &load,
			LookupPolicy policy) const;

	// mount_runtime's mount itself (the archives, the expansion that took).
	Error mount_runtime_archives_(const String &path, const String &expansion,
	                              bool allow_loose_override, const String &game_code);

	String expansion_;
	String game_code_; // game_code()
	// The expansion's text-override table the last mount_runtime left (see
	// get_expansion_override_table).
	PackedByteArray expansion_override_table_;

protected:
	static void _bind_methods();

public:
	static bool is_valid_root(const String &path);

	// Loose-source mount: PFF archives are ignored. The explicit --loose-root fallback and
	// format/runtime fixtures use this path over an unpacked game-data tree.
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
	// Global cache epoch (see base/resource_index/resource_index.h): bumped by every mount/clear on ANY
	// root. GDScript cache holders compare it against the epoch they were built under and
	// self-clear when it moved. bump_cache_epoch() lets tools/tests force an
	// invalidation after files change on disk without remounting.
	static int64_t cache_epoch();
	static void bump_cache_epoch();
	// The expansion this root ACTUALLY mounted ("" for base game, loose mounts, and
	// after mount_runtime's silent base fallback for an expansion that is not installed).
	// Feeds the expansion bank slots and the M<exp>/G<exp> music forms
	// [orig: Expansion_LoadAssets @ 0x4a4730, see docs/vfs/vfs-pff-mount-re.md]. Never reports the requested name back: a
	// caller that must not run on the wrong data set (the LAN joiner reconciling against the
	// host's authoritative expansion, D-NET-178) needs this to be evidence, not an echo.
	String get_expansion() const;
	// True only while a successful mount_runtime() mount is live: this root's data is the
	// packed game install. Loose mounts (set_root_dir), never-mounted roots, clear(), and
	// every failed mount report false. A caller re-mounting a root it did not create (the LAN
	// joiner switching an injected runtime root to the host's expansion, D-NET-178) reads this
	// to pick the right entry point: re-mounting a runtime root through set_root_dir would
	// silently drop its archives and leave the session on loose files.
	bool is_runtime_mount() const;
	// The expansion's text-override table the last mount_runtime() left: the loose
	// expansion/<n>/<n>.bin's bytes, or empty (no expansion, no loose file, a loose
	// mount, a failed mount). The rule is the engine's vfs_expansion_override_table: a
	// mount over a root that is not runtime-mounted is the boot's load (no archive
	// open), a mount over a runtime-mounted one the in-place switch's (the old archives
	// open: the loose file only under `/d`). Every mount_runtime() and set_root_dir()
	// emits `mounted` once it is done, so a holder re-reads this.
	PackedByteArray get_expansion_override_table() const;

	// Expansion names discoverable under `<path>/expansion/` (each subdir with a matching
	// <name>.pff). Independent of the currently mounted root, so the UI can list before mounting.
	PackedStringArray list_expansions(const String &path) const;
	// The expansion's own EXP_NAME / EXP_DESC (with the scan's fallbacks) out of
	// <name>.bin under <path>/expansion/<name>/ — engine vfs_expansion_info, which
	// resolves the .bin independently of the mounted stack.
	String expansion_name(const String &path, const String &expansion) const;
	String expansion_description(const String &path, const String &expansion) const;
	String get_root_dir() const;
	String get_last_error() const;
	void clear();

	String resolve_file(const String &name);
	PackedStringArray list_files(const String &suffix = String()) const;
	Array list_file_entries(const String &suffix = String()) const;
	// The gore-set extension the effect catalog loads alongside every `.ptl` on this
	// mount — ".ptg" when `fgn2.bin` is present, else ".ptu" (engine/base
	// ResourceIndex::particle_extension owns the witness).
	String particle_extension() const;
	// The effect catalog's files in parse order, less the names the archives carry only
	// stamped 0 (engine ResourceIndex::effect_files owns the witness, D-VFS-13).
	PackedStringArray effect_files() const;
	// Runtime roots honor the retail per-call source policy. Loose roots resolve
	// only flat loose files for every policy value.
	bool has_file(const String &name, LookupPolicy policy = LOOKUP_SESSION_DEFAULT) const;
	PackedByteArray read_file(const String &name, LookupPolicy policy = LOOKUP_SESSION_DEFAULT) const;
	// The texture `loader` makes of `name` (mip chain generated; cached per epoch),
	// null when nothing it opens decodes. No loader reads an alternate name.
	Ref<Texture2D> load_texture(const String &name, TextureLoader loader,
			LookupPolicy policy = LOOKUP_SESSION_DEFAULT) const;
	// C++ siblings only (not bound): the same load's decoded RGBA8 image, no
	// mips, uncached, for a device that uploads it itself (the HUD, the menus);
	// `r_alpha_only` reports whether the HUD loader resolved alpha mode (its
	// ".FULL" / ".ALPHA" suffixes override the caller's), which picks the
	// material the device draws it with.
	Ref<Image> load_texture_image(const String &name, TextureLoader loader,
			LookupPolicy policy = LOOKUP_SESSION_DEFAULT, bool *r_alpha_only = nullptr) const;
	// One material row's texture of runtime `type`: the one file retail's loader
	// opens for it, decoded by that loader's reader and prepared as the
	// dispatcher does; the checkerboard when it does not load.
	Ref<Texture> load_material_texture(const String &name, uint8_t type) const;
	Ref<Resource> load_font(const String &name) const;

	// The witnessed boot-required manifest (ENG-6, engine/base/gameprofile
	// required_resources; witness source docs/required-resources.md).
	// list_missing_boot_resources probes the FATAL-class file rows against
	// this mounted root and returns the missing names (empty = boot-viable).
	// The boot-archive-table trio is excluded: its all-missing gate is
	// enforced by mount_runtime itself [orig: PFF_OpenAllArchives @ 0x4a4310;
	// fatal check @ 0x4a6f44, see docs/vfs/vfs-pff-mount-re.md]. boot_resource_failure_text quotes the
	// witnessed failure behavior for a manifest name ("" for unknown names)
	// so owners can raise honest missing-resource errors.
	PackedStringArray list_missing_boot_resources() const;
	String boot_resource_failure_text(const String &name) const;
	// The text the boot report's line puts before a missing file's name, the one the editor's
	// Play reads the name back by (gameprofile::kBootResourceMissingMarker).
	static String boot_resource_missing_marker();

	// C++ siblings only (not bound): direct access to the mounted index without
	// Variant-boxing its rows through GDScript dictionaries.
	const opennova::ResourceIndex &native_index() const { return index_; }
	const opennova::assets::AssetStore &native_assets() const { return assets_; }
};

} // namespace godot

VARIANT_ENUM_CAST(godot::ResourceRoot::LookupPolicy);
VARIANT_ENUM_CAST(godot::ResourceRoot::TextureLoader);
