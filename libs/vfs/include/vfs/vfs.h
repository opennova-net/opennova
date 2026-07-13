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
//   Packed                 - PFF archives only; loose search paths ignored. The shipping
//                            runtime: the packed game data is the sole source.
//   PackedWithLooseOverride - both, loose shadowing archives. The runtime under the `/d`
//                            dev flag, where loose files override the packed data.
enum class VfsMountMode { LooseOnly, Packed, PackedWithLooseOverride };

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

// Engine-faithful virtual file system. Resolution precedence (HIGH -> LOW) mirrors
// Jointops.exe FileSystem_OpenFile @ 0x75b1c0: ordered loose search paths, then the primary
// archive, then ordered secondary archives. Loose files always shadow archived ones; among
// archives the primary wins, then secondaries in mount order. Lookup is by flat (basename),
// case-insensitive filename. See notes/vfs/phase0_ida_verification.md.
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
    // PFF_OpenAllArchives @ 0x4a4310. LooseOnly accepts an expansion directory without an
    // archive; packed modes require at least one usable base archive. A missing/unknown
    // expansion gracefully falls back to base-game mounting. `mode` selects
    // which layers are mounted (loose, archives, or both) — see VfsMountMode.
    bool mount_game(const std::string &game_root, const std::string &expansion = std::string(),
                    VfsMountMode mode = VfsMountMode::PackedWithLooseOverride,
                    VfsArchiveDiscovery discovery = VfsArchiveDiscovery::RetailTable);

    void clear();

    // Choose how read_file keys SCR payloads. Pass a VfsScrPolicy / gameprofile ScrPolicy value
    // (they share ordinals). Defaults to version-detect; persists across mounts. The game-aware
    // caller (runtime launch flag, importer) sets this so demo-vs-retail keying is correct.
    void set_scr_policy(int scr_policy);

    // --- Resolution (flat, case-insensitive filename) ---
    bool has_file(const std::string &name) const;
    bool read_file(const std::string &name, std::vector<uint8_t> &out) const;      // + SCR/BFC1 decode
    bool read_file_raw(const std::string &name, std::vector<uint8_t> &out) const;  // stored bytes only

    // Every resolvable logical name with its winning source, sorted by name.
    std::vector<VfsFileLocation> list_files() const;

    const std::string &game_root() const;     // root passed to mount_game ("" if unset)
    const std::string &last_error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// List expansion subdirectories under <game_root>/expansion that contain a <name>.pff
// (i.e. the expansions mount_game accepts). Returns the bare expansion names.
std::vector<std::string> vfs_list_expansions(const std::string &game_root);

// List every immediate expansion subdirectory. Loose authoring sessions do not need a
// matching archive, so this is deliberately broader than retail expansion discovery.
std::vector<std::string> vfs_list_loose_expansions(const std::string &game_root);

} // namespace opennova

#endif // OPENNOVA_VFS_H
