// weapon.sav — the retail Joint Operations player-profile weapon file.
//
// Five profile-slot records, each holding a BLUE and a RED side block plus a
// single-player kit page. A side block carries the profile's player CLASS byte
// (5 medic, 6 sniper, 7 gunner, 8 rifleman, 9 engineer), its avatar selection,
// and FIVE 2048-byte kit pages — one per class. In a multiplayer session the
// engine reads the side's class byte and uses that ONE integer to select both
// the wire class byte and the kit page it submits, which is why a retail
// loadout submit is always class-legal
// [orig: Game_StartMission @0x525767-0x525836; NapiNPClientMsg_TeamAssign
//  @0x431910; PlayerProfile_LoadAllFromDisk @0x54f4d0].
//
// File path resolution belongs to the caller: the original builds
// "expansion\<g_ExpansionName>\weapon.sav" when an expansion is mounted and
// plain "weapon.sav" otherwise [orig: @0x54f68c-@0x54f6b7].
//
// Native C++ static-link interface (ADR 0024). Godot-free, depends on
// engine/base/io only.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::playersav {

// Record geometry [orig: PlayerProfile_LoadAllFromDisk @0x54f4d0 — the
// `for (p = &g_CharSelClass; p < end; p += 67596) File_Read(p, 0x1080C)` loop].
inline constexpr size_t kProfileSlots           = 5;
inline constexpr size_t kRecordBytes            = 0x1080C;  // 67596
inline constexpr size_t kSideBytes              = 0x8006;   // 32774
inline constexpr size_t kKitPageBytes           = 2048;
inline constexpr size_t kKitPagesPerSide        = 5;        // classes 5..9
inline constexpr size_t kFirstKitPageOffset     = 6;
inline constexpr size_t kSinglePlayerPageOffset = 2 * kSideBytes;  // 65548
inline constexpr size_t kHeaderBytes            = 16;
inline constexpr uint8_t kMinPlayerClass        = 5;
inline constexpr uint8_t kMaxPlayerClass        = 9;

enum class SideId : uint8_t { Blue = 0, Red = 1 };

// The side a wire team byte selects: teams 1 and 3 take the BLUE block, every
// other value takes RED [orig: Game_StartMission @0x525798-0x5257b4; the same
// test at NapiNPClientMsg_TeamAssign @0x431a35].
SideId side_for_team(uint8_t team);

// The on-disk profile path RULE, relative to the game root: with an active
// expansion retail looks ONLY under that expansion's directory (no base-game
// fallback leg), otherwise the root's copy
// [orig: PlayerProfile_LoadAllFromDisk @0x54f4d0, path build
// @0x54f68c-@0x54f6b7 — g_ExpansionName[0] ? "expansion\<name>\weapon.sav"
// : "weapon.sav"]. Forward-slash separators; the embedder joins onto its
// mount root.
std::string weapon_sav_relpath(const std::string &expansion_name);

// One kit row. A page is consumed four tokens at a time as
// (name, ammo_primary, ammo_secondary, flags); the three numbers are decimal
// TEXT on disk and are truncated to their low byte on the wire, so -1 submits
// as 0xFF [orig: NetPacket_SendLoadoutSubmit @0x42CDC0 @0x42cf7c/@0x42cfbc/
// @0x42cff7].
//
// Modeling note: a page that stores only a bare weapon name and a page that
// stores an explicit "<name> -1 -1 -1" group decode to the SAME KitEntry,
// because a missing value defaults to -1. The writer re-emits the explicit
// form. Every page the original writes carries the explicit values (its
// defaults are whole "<name> -1 -1 -1" pages, below), so write() reproduces
// retail-written files byte for byte.
struct KitEntry {
    std::string name;
    int32_t ammo_primary = -1;
    int32_t ammo_secondary = -1;
    int32_t flags = -1;
};

struct KitPage {
    std::vector<KitEntry> entries;
};

// A side block: 6 bytes of header, five kit pages, then an unwitnessed tail.
struct Side {
    uint8_t player_class = 8;  // +0, 5..9 [orig: clamp @0x5516d0-@0x5516ec]
    uint8_t avatar_a = 0;      // +1 [orig: EntitySlot_LookupAndPackEntry out-param @0x54bbea]
    uint8_t avatar_b = 0;      // +2
    uint16_t avatar_packed = 0;  // +4, the packed avatar id
    // Class c lives at +6 + 2048*(c-5) [orig: the Game_StartMission switch
    // @0x5257c8 -> {6, 0x806, 0x1006, 0x1806, 0x2006}].
    std::array<KitPage, kKitPagesPerSide> pages;

    // nullptr when player_class is outside [5,9].
    const KitPage *page_for_class(uint8_t player_class) const;
    const KitPage *selected_page() const;
};

struct Record {
    Side blue;
    Side red;
    // +65548, the single-player kit page [orig: byte_256113C, read at @0x5246a8
    // and @0x5519c3 with default_single_player_page() as the fallback].
    KitPage single_player;

    const Side &side(SideId s) const;
    Side &side(SideId s);
};

struct File {
    // Header dwords 2 and 3, the header player.sav carries too: the save
    // writes the same 16 bytes at the head of both files, the flags word's
    // bit 0 the latch the player.sav load raises and the extra word's high
    // byte the byte it accumulates (formats/playersav/player_sav.h); the
    // weapon.sav load reads them and keeps neither [orig:
    // PlayerProfile_SaveToFiles @0x54beec; PlayerProfile_LoadAllFromDisk
    // @0x54f6e7]. Carried so a rewrite preserves them.
    uint32_t flags = 0;
    uint32_t extra = 0;
    std::array<Record, kProfileSlots> slots;
};

// Parse. Returns false on a short buffer or a bad magic/version. The header is
// accepted only when magic == "FPBC" and version == "0211"
// [orig: PlayerProfile_LoadAllFromDisk @0x54f4d0].
// Tolerates a file with fewer than kProfileSlots records only if it is exactly
// kHeaderBytes long (empty) — the slots then hold the struct defaults.
bool read(const uint8_t *data, size_t size, File &out);

// Serialize FROM SCRATCH (docs/adr/0003-no-raw-passthrough-create-from-scratch.md):
// every byte is produced by this writer, never copied through from a parsed
// input. Always kHeaderBytes + kProfileSlots * kRecordBytes bytes.
std::vector<uint8_t> write(const File &in);

// The default kit pages: the program's eleven static double-NUL page blobs,
// each a whole "<name> -1 -1 -1" kit, which PlayerProfile_InitDefaults copies
// into every record [orig: @0x54bced (single player, 0x833BF8), the blue set
// @0x54bd7f..0x54bdd1 (0x8353F8, 0x8363F8, 0x8373F8, 0x8393F8, 0x83A3F8), the
// red set @0x54bd1a..0x54bd6c (0x835BF8, 0x836BF8, 0x837BF8, 0x839BF8,
// 0x83ABF8)]. The single-player page is also the session start's fallback
// when neither the mission nor the profile carries one [orig:
// Game_StartMission @0x5246be; Game_ApplySessionSettingsToGlobals @0x5519e4].
KitPage default_single_player_page();
// nullptr-free: a class outside [5,9] returns an empty page.
KitPage default_kit_page(SideId side, uint8_t player_class);

// The defaults when weapon.sav is absent, the record half of
// [orig: PlayerProfile_InitDefaults @0x54bb40]: both sides class 8, the
// default kit pages above, the avatar bytes zero (the original seeds them from
// the character table, EntitySlot_LookupAndPackEntry @0x54bbea; the embedder
// that holds that table fills them).
File make_defaults();

// Apply the PLAYER_INFO ACCEPT header edits to one profile slot. Retail writes
// the selected class to BOTH side blocks, then writes nationality/division and
// the packed character id only to the currently selected side
// [orig: PlayerInfo_SaveFromDialog @0x55EE3F-0x55EE6D (class loop),
//  @0x55EE93-0x55EF38 (selected-side bytes +1/+2/+4)]. The caller owns file
// persistence; untouched slots, kit pages, and the other side's avatar remain
// unchanged. Returns false for an out-of-range slot.
bool update_avatar_selection(File &file, size_t slot, SideId selected_side,
        uint8_t player_class, uint8_t avatar_a, uint8_t avatar_b,
        uint16_t avatar_packed);

// Clamp every side's class byte into [5,9], per side independently
// [orig: Game_ApplySessionSettingsToGlobals @0x5516d0-@0x5516ec].
void clamp_classes(File &f);

// The boot-time profile image -> the live record [orig:
// PlayerProfile_LoadAllFromDisk @0x54f4d0]: `slot0` starts at the shipped
// defaults [orig: PlayerProfile_InitDefaults @0x54bb40]; a readable profile
// (the "FPBC"/"0211" header gate) replaces it with its first slot, classes
// clamped into [5,9] the way session start does [orig:
// Game_ApplySessionSettingsToGlobals @0x5516ab..@0x5516ec]. False = the bytes
// were not a profile and the defaults stand. The embedder reads the file:
// weapon.sav is a SAVE file on the OS filesystem, never a PFF/mount entry.
bool profile_or_defaults(const uint8_t *data, size_t size, Record &slot0);

// Page blob codec [orig: Buffer_CopyUntilDoubleNull @0x562f30]. NUL-separated
// ASCII terminated by an empty string (a double NUL) or by the end of the page.
// Exposed for tests.
KitPage decode_kit_page(const uint8_t *page, size_t size);
void encode_kit_page(const KitPage &page, uint8_t *out, size_t size);

}  // namespace opennova::playersav
