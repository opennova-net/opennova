#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <base/vfs/file_source.h>
#include <base/vfs/vfs.h>

namespace opennova {

struct LaunchFlags;

// Process-wide refresh for derived resource caches. ResourceRoot advances this
// on mount/clear and explicit refresh; native asset consumers observe it too.
// Individual ResourceIndex revisions also cover mounts in headless embedders.
uint64_t cache_epoch();
void bump_cache_epoch();

struct ResourceFileEntry {
	std::string kind;
	std::string path;
	std::string logical_name;
	std::string display_name;
	std::string relative_path;
	std::string source_type;
	std::string archive_path;
	// File metadata, filled at scan time for loose entries. Archive (.pff) entries
	// leave these zero because the location carries no size/time. size_bytes is
	// the on-disk byte count; modified_time is the last-write time in Unix
	// seconds (UTC), or 0 when unknown.
	uint64_t size_bytes = 0;
	int64_t modified_time = 0;
};

class ResourceIndex {
public:
	ResourceIndex();
	~ResourceIndex();

	ResourceIndex(ResourceIndex &&) noexcept;
	ResourceIndex &operator=(ResourceIndex &&) noexcept;

	ResourceIndex(const ResourceIndex &) = delete;
	ResourceIndex &operator=(const ResourceIndex &) = delete;

	// Mount and index a game install. When `expansion` is non-empty and
	// <root>/expansion/<name>/<name>.pff exists, the expansion's loose files + archives
	// override the base game (see opennova::Vfs::mount_game). Empty expansion = base game.
	// `mode` selects which layers are mounted (loose, archives, or both) — see VfsMountMode.
	// `discovery` selects base-archive discovery. The catalog defaults to ScanAll
	// so explicitly mounted mod roots can expose arbitrary archives; boot callers
	// pass RetailTable, the witnessed fixed table
	// [orig: PFF_OpenAllArchives @ 0x4a4310].
	// Scanning an already-mounted index REPLACES the mount outright (clear() first: archives
	// closed, search paths dropped, entries rebuilt) — it never layers onto the previous one,
	// so a remount in place is how a live root moves to another expansion.
	bool scan(const std::string &root_dir, const std::string &expansion = std::string(),
	          VfsMountMode mode = VfsMountMode::PackedWithLooseOverride,
	          VfsArchiveDiscovery discovery = VfsArchiveDiscovery::ScanAll);
	// What scan_install came to: the install mounted and indexed; the root mounted but opened
	// none of the game's archives, which retail's boot refuses (last_error() names an archive
	// that failed to open, when one did: a corrupt sole archive is this, not Unmounted); or
	// the root did not mount at all (last_error() says why).
	enum class InstallScan { Mounted, NoArchive, Unmounted };
	// Mount and index a game install as a launch with `flags` mounts it (mount_install,
	// boot_policy.h: the witnessed boot table with the /exp expansion over it, the /game
	// code's key, the archives alone unless /d puts the loose files first). Replaces the
	// mount as scan() does.
	InstallScan scan_install(const std::string &root_dir, const LaunchFlags &flags);
	// Mount an embedder's own file set in place of an install (the editor's project files, the
	// documents it has open standing in for theirs: base/vfs/file_source.h), replacing the mount as
	// scan() does. One flat namespace, the source's own lookup: has_file is a name the source
	// resolves (its stamp is not 0), read_file its read; a lookup policy changes nothing (there is
	// no archive under a loose file), no file is preferred loose, nothing is indexed by kind
	// (resource_files answers none: a consumer reads the names it knows), and root_dir() is
	// kSourceRootDir, a label and no directory. The index reads the source as it stands at each
	// call; a holder that caches what it read asks the source for the name's stamp. False, the
	// index left cleared, for no source.
	static constexpr const char *kSourceRootDir = "source:";
	bool mount_source(std::shared_ptr<const FileSource> files);
	// The mounted files changed where the index cannot see it (a file source one of whose stamps
	// moved): the revision moves, so a holder of what it parsed from the index parses it again.
	void mark_changed() { ++revision_; }
	void clear();
	// Mount/decode revision, including failed scans.
	uint64_t revision() const { return revision_; }
	bool has_mounted_archive() const;
	bool prefers_loose_file(const std::string &name) const;
	// Vfs::loose_first_hit: the loose-first answer under a per-call policy.
	bool loose_first_hit(const std::string &name, VfsLookupPolicy policy) const;

	// Choose how read_file keys SCR payloads (forwards to the underlying Vfs). Pass a
	// gameprofile ScrPolicy / VfsScrPolicy value; defaults to version-detect and persists
	// across scans. Set by the game-aware caller so demo-vs-retail keying is correct.
	void set_scr_policy(int scr_policy);

	std::vector<ResourceFileEntry> resource_files(const std::string &kind) const;
	// The gore-set extension the effect catalog loads ALONGSIDE every `.ptl`: ".ptg"
	// when this mount carries `fgn2.bin` (the German content marker), else ".ptu".
	// The `particle` kind indexes all three extensions; this is the runtime SELECTION
	// [orig: Game_LoadConfig @ 0x5514e8..0x5514fa -> byte_24D4DF9, read by
	// CEffectSystem_Init @ 0x5f608b..0x5f6095]. Empty mount answers the ".ptu" default.
	std::string particle_extension() const;
	// The effect catalog's files, the order the runtime parses them: every mounted `.ptl`,
	// then every file of the gore set (particle_extension()), each list by name. A name the
	// mounted archives carry only in entries stamped 0 is left out: retail's effect loader
	// walks each archive's directory and skips an entry whose +12 word is 0, so such a file
	// is never parsed [orig: CEffectSystem_Init @ 0x5f6070 — the slot walk @ 0x5f6485, the
	// skip @ 0x5f64c0, the extension test @ 0x5f64cd..0x5f64f3] (D-VFS-13). A name no
	// archive carries (a loose file) stays.
	std::vector<std::string> effect_files() const;
	// The archive slots retail's walks visit (Vfs::archive_slot_entries / _has_file).
	std::vector<VfsArchiveEntry> archive_slot_entries(int slot) const;
	bool archive_slot_has_file(int slot, const std::string &name) const;
	// Default overloads use the session policy selected by scan(); policy overloads
	// let retail consumers force one lookup without mutating that session default.
	bool has_file(const std::string &name) const;
	bool has_file(const std::string &name, VfsLookupPolicy policy) const;
	bool read_file(const std::string &name, std::vector<uint8_t> &out) const;
	bool read_file(const std::string &name, std::vector<uint8_t> &out, VfsLookupPolicy policy) const;
	const std::string &root_dir() const;
	// The expansion whose layers ACTUALLY mounted, empty for base game — including after
	// scan()'s silent fallback for a missing/unknown expansion (opennova::Vfs::mount_game).
	// Read this, never the requested name, when running on the wrong data set is a bug
	// (the LAN joiner's host-expansion reconcile, D-NET-178).
	const std::string &mounted_expansion() const;
	const std::string &last_error() const;

private:
	// Index the files the Vfs mounted (scan and scan_install, after their mount).
	void index_mounted();

	uint64_t revision_ = 0;
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

// The effect catalog's files in the order the runtime parses them, of `names` (logical names):
// every `.ptl`, then every file of the gore set (`gore_extension`, ResourceIndex::particle_extension),
// each list by name without case; a name of neither is left out. ResourceIndex::effect_files' order,
// for an embedder whose files no index lists (the OpenNova Editor's project, ADR 0046 DI-14)
// [orig: CEffectSystem_Init @ 0x5f6070 — loose `ptl\*.ptl` @ 0x5f6228, then `ptl\*<ext>` @ 0x5f6356,
// the archive walk's extension test @ 0x5f64cd..0x5f64f3].
std::vector<std::string> effect_file_order(const std::vector<std::string> &names, const std::string &gore_extension);

// The German content marker whose mere presence in the mount picks the gore set.
inline constexpr const char *kGoreContentMarker = "fgn2.bin";

// The gore set's extension the effect catalog loads alongside every `.ptl`: ".ptg" where the mount
// carries kGoreContentMarker, else ".ptu". Retail picks it once at config time from the file's
// PRESENCE and never re-reads it [orig: Game_LoadConfig @ 0x551480 sets byte_24D4DF9 =
// FileSystem_FileExists("fgn2.bin") != 0 @0x5514e8..0x5514fa; CEffectSystem_Init @ 0x5f6070 reads it
// to pick ".ptg" over the ".ptu" default @0x5f608b..0x5f6095]. ResourceIndex::particle_extension is
// this over its own mount; an embedder with its own file set (a mission's boot files, the OpenNova
// Editor's project) asks its own.
inline const char *gore_particle_extension(bool has_gore_content_marker) {
	return has_gore_content_marker ? ".ptg" : ".ptu";
}

} // namespace opennova
