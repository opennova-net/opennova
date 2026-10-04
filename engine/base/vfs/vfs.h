#pragma once

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

// The secondary-archive slot table mount_game fills under RetailTable discovery
// [orig: PFF_OpenAllArchives @ 0x4a4310 over the name table @ 0x829f90, whose count
// g_PffArchiveNameCount @ 0x82a5a8 is 6, -> FileSystem_SetSecondaryArchive; read back by
// FS_GetSecondaryArchiveByIndex @ 0x75ad60]: slot 0 <n>L.pff and slot 1 <n>.pff
// (Expansion_LoadAssets @ 0x4a48ed / @ 0x4a48d6), then the boot table; slot 5 is never
// written. Slot order is the order retail's archive walks visit them.
inline constexpr int kArchiveSlotCount = 6;
inline constexpr int kArchiveSlotExpansionText = 0; // <n>L.pff
inline constexpr int kArchiveSlotExpansion = 1;     // <n>.pff
inline constexpr int kArchiveSlotLanguage = 2;      // language.pff
inline constexpr int kArchiveSlotLocalres = 3;      // localres.pff
inline constexpr int kArchiveSlotResource = 4;      // resource.pff

// One directory entry of a mounted archive, as its table stores it.
struct VfsArchiveEntry {
    std::string name;       // entry name, original case
    uint32_t timestamp = 0; // the +12 word: a Unix time in every retail entry
};

// How the mounted archives stamp a name's directory entries. The game's two effect loaders
// walk each archive's directory and skip an entry whose +12 word is 0
// [orig: CEffectSystem_Init @ 0x5f64c0; HLSLEffect_LoadAllFromPFFArchive @ 0x5aff26].
enum class VfsArchiveStamp {
    NotArchived, // no mounted archive carries the name
    Unstamped,   // every mounted entry of the name is stamped 0
    Stamped,     // at least one mounted entry of the name carries a nonzero stamp
};

// Engine-faithful virtual file system. The flat overloads provide a basename,
// case-insensitive index where mounted loose paths shadow the
// primary archive and then ordered secondaries. The policy overloads mirror retail
// FileSystem_OpenFile @ 0x75b1c0: they retain the full relative query and choose loose/archive
// order per call. Packed mode is archive-default while an archive is online and keeps loose
// roots available for ForceLooseFirst; its standalone no-archive default falls back loose, while
// the game runtime rejects that state via has_mounted_archive(). See
// docs/vfs/vfs-pff-mount-re.md.
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
    // Whether the current session allows this exact loose query to win.
    bool prefers_loose_file(const std::string &name) const;
    // Whether a loose-first search under `policy` finds this exact query as a loose
    // file: the session's answer (prefers_loose_file) by default, the loose file's
    // existence under ForceLooseFirst, never under ForceArchiveOnly. The loaders that
    // let a loose file beat a .dds sibling ask it with the caller's policy.
    bool loose_first_hit(const std::string &name, VfsLookupPolicy policy) const;

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

    // The directory of the archive in retail slot `slot` (kArchiveSlotCount), in the
    // archive's table order (sorted by upper-cased name at open, as PFF_Open sorts it);
    // empty when the slot holds no archive. Only mount_game's RetailTable discovery fills
    // slots: ScanAll and the general mount API leave every slot empty.
    std::vector<VfsArchiveEntry> archive_slot_entries(int slot) const;
    // Whether the archive in `slot` holds `name`, matched as the archive lookup matches it
    // [orig: PFF_FileExists @ 0x768680 -> PFF_FindEntry @ 0x7685d0]; false for an empty slot.
    bool archive_slot_has_file(int slot, const std::string &name) const;
    // How the mounted archives (every one, slotted or not) stamp `name`'s entries, matched
    // by the flat case-insensitive name.
    VfsArchiveStamp archive_stamp(const std::string &name) const;

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

// An expansion's own name and description, as retail's expansion scan reads them: the
// [exp_info] EXP_NAME / EXP_DESC entries of <name>.bin, each with its own fallback. The
// scan resolves the .bin independently of the mounted stack — a loose file under
// expansion/<name>/ first (that search path is loose-first), then the L archive (the LAST
// primary the scan installs), then the base archive — so an unmounted expansion reads
// the same way; a missing or unparseable .bin yields both fallbacks.
// [orig: Expansion_ScanAndRegister @ 0x4a43d0 — loose-first search path @ 0x4a446a,
//  <n>.pff primary @ 0x4a44cd then <n>L.pff primary @ 0x4a450a,
//  TextResource_LoadFile("<n>.bin") @ 0x4a455b, EXP_NAME @ 0x4a4578 else
//  "Unnamed Expansion" @ 0x4a45c2 (and the no-.bin arm @ 0x4a4670), EXP_DESC @ 0x4a45ef
//  else "This expansion lacks a description." @ 0x4a4648 / @ 0x4a46b2]
struct ExpansionInfo {
    std::string name;
    std::string description;
};
inline constexpr const char *kExpansionUnnamed = "Unnamed Expansion";
inline constexpr const char *kExpansionNoDescription = "This expansion lacks a description.";
ExpansionInfo vfs_expansion_info(const std::string &game_root, const std::string &expansion);

// Where Expansion_LoadAssets runs, which decides what its override-table load
// reaches (vfs_expansion_override_table).
enum class ExpansionLoadPoint {
    // Boot and the full reload: no archive is open [orig: Game_InitSubsystems —
    // the scan closed its archives @ 0x4a46c4..0x4a46db, Expansion_LoadAssets
    // @ 0x4a6f37, then PFF_OpenAllArchives @ 0x4a6f44; Game_ReloadExpansionAndMods
    // — PFF_CloseAllOpenArchives @ 0x552770, then @ 0x552775].
    ArchivesClosed,
    // The menu's and the join's switch: the previous set is still open
    // [orig: Expansion_ReloadAllAssets @ 0x5683a9, closed after @ 0x5683ae;
    // Expansion_SwitchTo @ 0x568963, closed after @ 0x568968].
    ArchivesOpen,
};

// The expansion's text-override table [orig: g_TextOverrideTable @ 0x33429b0]
// into `out`: the bytes of the loose expansion/<name>/<name>.bin, the only file
// that serves it. Expansion_LoadAssets asks File_LoadResource for
// "expansion\<n>\<n>.bin" [orig: @ 0x4a49d4 -> TextResource_LoadOverrideTable
// @ 0x75d5c0 -> File_LoadResource @ 0x75b540], which asks an archive for the
// whole query (the basename strip's setter @ 0x75a590 has no caller, and
// PFF_FindEntry @ 0x7685d0 matches the archive's flat entry names), so an
// archived <n>.bin never serves it. With an archive open and loose-first off it
// walks only the archives (@ 0x75b56c..0x75b57c), so at ArchivesOpen the loose
// file is reached only under /d (`loose_first`: the session flag /d raises
// @ 0x4a6fac). The loose walk tries the search path's join first,
// expansion/<n>/expansion/<n>/<n>.bin (the path @ 0x4a49bb, the join
// @ 0x75b5b9), then the query itself (@ 0x75b5a4), both under `game_root`. False
// (no table: the old one freed, the global left NULL @ 0x75d5c7..0x75d5f6) for
// an empty expansion or one whose <n>.pff is absent (the name cleared
// @ 0x4a4775, the table cleared @ 0x4a482a), or when no loose file is reached;
// an empty file is no table either (the loader would fix pointers up over a
// zero-length allocation, a degenerate original path with no value to keep).
// docs/interface/rtxt-strings-re.md "The override table's source" (D-RTXT-10).
bool vfs_expansion_override_table(const std::string &game_root, const std::string &expansion,
                                  ExpansionLoadPoint point, bool loose_first,
                                  std::vector<uint8_t> &out);

// The expansion version-file CRC [orig: CRC_ComputeCustomTable @ 0x53c820]:
// MSB-first CRC-32, polynomial 0x04C11DB7, init -1, no reflection, no final
// xor ("CRC-32/MPEG-2"; the 256-entry table @ 0x830780 — entries [1]
// 0x04C11DB7 / [31] 0x745E66CD verified against the generated table). Retail's
// do-while consumes one byte even at length 0 (an out-of-bounds read no live
// caller reaches); this port loops size times and returns the -1 init for an
// empty buffer.
int32_t vfs_version_crc(const uint8_t *data, size_t size);

// g_ExpansionChecksum's producer [orig: Expansion_LoadAssets — the reset to 0
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

// The install's country code: the first two bytes of the game directory's
// CC.BIN (found case-insensitively), cut at a NUL; "" when the file is absent
// or empty. The joiner uploads it as the ClientAuth COUNTRYCODE, which retail
// omits when empty [orig: Game_ReadCCBinFile @ 0x4a5860 into byte_B4C4D8 from
// Game_InitSubsystems @ 0x4a6d96; UI_JoinSelectedSession @ 0x569b70;
// CNapiServerInfo_SerializeToSession @ 0x4c385a] (D-NET-296).
std::string vfs_country_code(const std::string &game_root);

} // namespace opennova
