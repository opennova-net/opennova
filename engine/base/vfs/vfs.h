#ifndef OPENNOVA_VFS_H
#define OPENNOVA_VFS_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace opennova {

enum class VfsSource { LooseDir, Archive };

// Which layers mount_game brings online.
//   LooseOnly              - loose search paths only; archives ignored. Used by
//                            explicit loose development roots.
//   Packed                 - archives back the session default and flat index while any are
//                            online. Loose roots remain latent for explicit ForceLooseFirst
//                            calls; standalone no-archive sessions fall back loose, while the
//                            game runtime rejects that state via has_mounted_archive().
//   PackedWithLooseOverride - both, loose shadowing archives. The runtime under the `/d`
//                            dev flag, where loose files override the packed data.
enum class VfsMountMode { LooseOnly, Packed, PackedWithLooseOverride };

// Per-query resolution order for the retail-faithful lookup overloads. Retail keeps a
// session default (/d selects loose-first) but selected callers temporarily force the
// loose or archive path for one open without changing that default.
// [orig: FileSystem_OpenFile @ 0x75b1c0; Terrain_LoadTileInfoFile @ 0x60a74e;
// Mission_LoadBMSFromPFF @ 0x40d43c]
enum class VfsLookupPolicy { SessionDefault, ForceLooseFirst, ForceArchiveOnly };

// How mount_game discovers base-root archives.
//   RetailTable - the witnessed fixed boot table: language.pff, localres.pff,
//                 resource.pff probed by name (case-insensitive), slot order =
//                 precedence; extra .pff files in the root NEVER mount
//                 [orig: PFF_OpenAllArchives @ 0x4a4310, name table @ 0x829f90].
//                 The game-shaped default.
//   ScanAll     - every base-root *.pff, alphabetical. An explicit catalog
//                 policy for arbitrary mod archives (docs/vfs/vfs-pff-mount-re.md
//                 D-VFS-2 records the decision).
enum class VfsArchiveDiscovery { RetailTable, ScanAll };

// The witnessed fixed boot table [orig: PFF_OpenAllArchives @ 0x4a4310, name
// table @ 0x829f90 stride 260]: after the expansion pair (slots 0/1), slot 2 =
// language.pff, 3 = localres.pff, 4 = resource.pff (slot 5 has no writer).
// Slot order IS lookup precedence; extra .pff files in the root never mount in
// retail. Any one of them present makes a directory a mountable game dir.
inline constexpr const char *kBootArchiveTable[] = {
    "language.pff",
    "localres.pff",
    "resource.pff",
};

struct VfsFileLocation {
    std::string logical_name;                 // entry name, original case
    VfsSource source = VfsSource::LooseDir;
    std::string source_path;                  // loose directory path, or archive (.pff) path
    int precedence = 0;                       // 0 = highest priority; grows down the stack
};

// Engine-faithful virtual file system. The flat overloads provide a basename,
// case-insensitive index where mounted loose paths shadow the
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
    // caller sets this so demo-vs-retail keying is correct.
    void set_scr_policy(int scr_policy);

    // --- Flat resolution (case-insensitive filename) ---
    bool has_file(const std::string &name) const;
    bool read_file(const std::string &name, std::vector<uint8_t> &out) const;      // + SCR/BFC1 decode
    bool read_file_raw(const std::string &name, std::vector<uint8_t> &out) const;  // stored bytes only

    // --- Retail per-query resolution ---
    // The full relative query is used for loose probes and archive comparison. Policy changes
    // only this call's search order; it never mutates the session default or flat index.
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

// The expansion version-file CRC [orig: CRC_ComputeCustomTable @ 0x53c820]:
// MSB-first CRC-32, polynomial 0x04C11DB7, init -1, no reflection, no final
// xor ("CRC-32/MPEG-2"; the 256-entry table @ 0x830780 — entries [1]
// 0x04C11DB7 / [31] 0x745E66CD verified against the generated table). Retail's
// do-while consumes one byte even at length 0 (an out-of-bounds read no live
// caller reaches); this port loops size times and returns the -1 init for an
// empty buffer.
int32_t vfs_version_crc(const uint8_t *data, size_t size);

// g_expansion_checksum's producer [orig: Expansion_LoadAssets — the reset to 0
// @ 0x4a4781, the loose expansion\<name>\version.txt size/load/CRC
// @ 0x4a4858..0x4a488a]. Returns 0 when the expansion is empty or the loose
// file is absent/unreadable; an EMPTY version.txt also returns 0 (retail would
// run its one-byte do-while off the end of a zero-length allocation — a
// degenerate original path with no stable value to reproduce). The joiner
// formats this value as signed decimal into the JOIN `VERSIONCRCSTRING` TLV
// and an expansion host compares it against its own [orig: the "%ld" sprintf
// @ 0x42a287; the atol compare @ 0x512331, reject DPC=48 @ 0x512341]
// (D-NET-166).
int32_t vfs_expansion_version_checksum(const std::string &game_root,
                                       const std::string &expansion);

} // namespace opennova

#endif // OPENNOVA_VFS_H
