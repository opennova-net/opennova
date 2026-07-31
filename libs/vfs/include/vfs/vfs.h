#ifndef OPENNOVA_VFS_H
#define OPENNOVA_VFS_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace opennova {

enum class VfsSource { LooseDir, Archive };

// Which layers mount_game brings online.
//   LooseOnly              - loose search paths only; archives ignored. The editor authors
//                            loose files and never reads PFFs.
//   Packed                 - archives back the session default and legacy index while any are
//                            online. Loose roots remain latent for explicit ForceLooseFirst
//                            calls; standalone no-archive sessions fall back loose, while the
//                            game runtime rejects that state via has_mounted_archive().
//   PackedWithLooseOverride - both, loose shadowing archives. The runtime under the `/d`
//                            dev flag, where loose files override the packed data.
enum class VfsMountMode { LooseOnly, Packed, PackedWithLooseOverride };

// Per-query resolution order for the retail-faithful lookup overloads. Retail keeps a
// session default (/d selects loose-first) but selected callers temporarily force the
// loose or archive path for one open without changing that default.
// [orig: FileSystem_OpenFile @ 0x75b1c0; Terrain_LoadFoliageFile @ 0x60a74e;
// Mission_LoadBMSFromPFF @ 0x40d43c]
enum class VfsLookupPolicy { SessionDefault, ForceLooseFirst, ForceArchiveOnly };

// How mount_game discovers base-root archives.
//   RetailTable - the witnessed fixed boot table: language.pff, localres.pff,
//                 resource.pff probed by name (case-insensitive), slot order =
//                 precedence; extra .pff files in the root NEVER mount
//                 [orig: PFF_OpenAllArchives @ 0x4a4310, name table @ 0x829f90].
//                 The game-shaped default (runtime, importer, C ABI).
//   ScanAll     - every base-root *.pff, alphabetical. A deliberate authoring
//                 divergence: the editor's browse index must see arbitrary
//                 archives a modder drops in (docs/vfs/vfs-pff-mount-re.md
//                 D-VFS-2 records the decision).
enum class VfsArchiveDiscovery { RetailTable, ScanAll };

struct VfsFileLocation {
    std::string logical_name;                 // entry name, original case
    VfsSource source = VfsSource::LooseDir;
    std::string source_path;                  // loose directory path, or archive (.pff) path
    int precedence = 0;                       // 0 = highest priority; grows down the stack
};

// Engine-faithful virtual file system. The legacy overloads preserve the authoring/importer
// model: a flat (basename), case-insensitive index where mounted loose paths shadow the
// primary archive and then ordered secondaries. The policy overloads mirror retail
// FileSystem_OpenFile @ 0x75b1c0: they retain the full relative query and choose loose/archive
// order per call. Packed mode is archive-default while an archive is online and keeps loose
// roots available for ForceLooseFirst; its standalone no-archive default falls back loose, while
// the game runtime rejects that state via has_mounted_archive(). See
// notes/vfs/phase0_ida_verification.md.
class Vfs {
public:
    Vfs();
    ~Vfs();
    Vfs(Vfs &&) noexcept;
    Vfs &operator=(Vfs &&) noexcept;
    Vfs(const Vfs &) = delete;
    Vfs &operator=(const Vfs &) = delete;

    // --- General mount API ---
    // (mirrors FileSystem_AddSearchPath / SetPrimaryArchive / SetSecondaryArchive)
    bool add_search_path(const std::string &dir);            // loose dir, highest precedence, add order
    bool set_primary_archive(const std::string &pff_path);    // single primary archive
    bool add_secondary_archive(const std::string &pff_path);  // appended in add order

    // Game-faithful auto-config. Mounts a game install the way the engine does. When
    // `expansion` is non-empty and <root>/expansion/<name>/<name>.pff exists, this adds
    // <root>/expansion/<name> as the top search path, the game root as the next search path
    // (the engine's CWD probe), <name>L.pff as the primary archive and <name>.pff as a
    // secondary, then the base-root archives per `discovery` (the witnessed fixed boot
    // table by default). Mirrors Expansion_LoadAssets @ 0x4a4730 +
    // PFF_OpenAllArchives @ 0x4a4310. Returns false only on a bad root; a
    // missing/unknown expansion gracefully falls back to base-game mounting. `mode` selects
    // which layers are mounted (loose, archives, or both) — see VfsMountMode. Use
    // has_mounted_archive() when the caller must reject an otherwise valid loose-only root.
    bool mount_game(const std::string &game_root, const std::string &expansion = std::string(),
                    VfsMountMode mode = VfsMountMode::PackedWithLooseOverride,
                    VfsArchiveDiscovery discovery = VfsArchiveDiscovery::RetailTable);

    void clear();

    // True when at least one primary or secondary archive opened successfully.
    bool has_mounted_archive() const;

    // Choose how read_file keys SCR payloads. Pass a VfsScrPolicy / gameprofile ScrPolicy value
    // (they share ordinals). Defaults to version-detect; persists across mounts. The game-aware
    // caller (runtime launch flag, importer) sets this so demo-vs-retail keying is correct.
    void set_scr_policy(int scr_policy);

    // --- Legacy resolution (flat, case-insensitive filename) ---
    // These overloads intentionally retain the authoring/importer compatibility model.
    bool has_file(const std::string &name) const;
    bool read_file(const std::string &name, std::vector<uint8_t> &out) const;      // + SCR/BFC1 decode
    bool read_file_raw(const std::string &name, std::vector<uint8_t> &out) const;  // stored bytes only

    // --- Retail per-query resolution ---
    // The full relative query is used for loose probes and archive comparison. Policy changes
    // only this call's search order; it never mutates the session default or legacy index.
    bool has_file(const std::string &name, VfsLookupPolicy policy) const;
    bool read_file(const std::string &name, std::vector<uint8_t> &out,
                   VfsLookupPolicy policy) const;      // + SCR/BFC1 decode
    bool read_file_raw(const std::string &name, std::vector<uint8_t> &out,
                       VfsLookupPolicy policy) const;  // stored bytes only

    // Every resolvable logical name with its winning source, sorted by name.
    std::vector<VfsFileLocation> list_files() const;

    const std::string &game_root() const;     // root passed to mount_game ("" if unset)
    // The expansion whose layers ACTUALLY mounted, not the one that was requested. Because
    // mount_game silently falls back to base-game mounting for a missing/unknown expansion,
    // a caller that must not run on the wrong data set (the LAN joiner reconciling against
    // the host's authoritative expansion — D-NET-178) cannot read the request back as proof;
    // this reports the truth. Empty = base game, including after a fallback.
    const std::string &mounted_expansion() const;
    const std::string &last_error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// List expansion subdirectories under <game_root>/expansion that contain a <name>.pff
// (i.e. the expansions mount_game accepts). Returns the bare expansion names.
std::vector<std::string> vfs_list_expansions(const std::string &game_root);

} // namespace opennova

#endif // OPENNOVA_VFS_H
