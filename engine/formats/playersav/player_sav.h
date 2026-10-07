// player.sav — the retail Joint Operations player profile file.
//
// Five profile records (the PLAYER_INFO screen's PLAYER list, the LAN screen's
// PLAYERCHOICE), each 0x3C80 bytes: the player's name, the single-player
// campaign table, the ten chat macros, the single-player session words, the
// control options and the key-binding table. The file is the 16-byte header
// weapon.sav shares, the five records, then an 8-byte trailer. It always lives
// beside the game, never under an expansion (weapon.sav does)
// [orig: PlayerProfile_SaveToFiles @0x54be00 — header @0x54be2e..0x54be43,
//  the 5 x 0x3C80 record writes @0x54be76, the trailer @0x54be95;
//  PlayerProfile_LoadAllFromDisk @0x54f4d0 — the "FPBC"/"0211" gate @0x54f5c6,
//  the record reads @0x54f5ec].
//
// Every offset below is one the original's code reads or writes; the regions no
// code touches (+56, +976..+1340, +1344, +1348, +1384..+1408, +1492,
// +1534..+1656, +1664..+1800, a binding entry's +2) are zero across the retail
// corpus and are neither stored nor passed through: the writer emits zeros.
// Witness record: docs/playerinfo/player-sav-re.md.
//
// Native C++ static-link interface (ADR 0024). Godot-free, depends on
// engine/base/io only.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <formats/playersav/weapon_sav.h>

namespace opennova::playersav {

inline constexpr size_t kPlayerRecordBytes = 0x3C80;  // 15488
inline constexpr size_t kTrailerBytes = 8;
inline constexpr size_t kPlayerSavBytes =
		kHeaderBytes + kProfileSlots * kPlayerRecordBytes + kTrailerBytes;  // 77464
// The trailer: the eight bytes of the program's literal "fsasyaof", which the
// save writes through its pointer [orig: PlayerProfile_SaveToFiles
// @0x54be95 — FileSystem_Write(off_829F3C, 8), off_829F3C -> 0x7C8C1C]. The
// load never reads it.
inline constexpr char kTrailer[kTrailerBytes + 1] = "fsasyaof";

// The header's flags word bit 0, which the load latches and the save writes
// back [orig: @0x54f5d1 -> dword_2540CD8; @0x54be41]; the extra word's high
// byte, which the load adds into a byte the save writes back as that high
// byte [orig: @0x54f653 `byte_252DE54 += HIBYTE(extra)`; @0x54be3d]. Nothing
// else reads either.
inline constexpr uint32_t kHeaderFlagLatched = 1;

// Each record's +0 dword, "0211" [orig: PlayerProfile_InitDefaults @0x54bb94].
inline constexpr uint32_t kRecordTag = 0x31313230u;

// The name field: +4 up to the flags dword at +52. The game writes at most 16
// bytes into it (GetUserNameA's 16-byte buffer @0x54f535, the name edit's
// strncpy @0x55fc91) and reads it with a 32- or 64-byte copy.
inline constexpr size_t kNameOffset = 4;
inline constexpr size_t kNameBytes = 48;
inline constexpr size_t kNameMaxChars = 16;
// +52 bit 0: the record has no name ("CS_NONAME" in the PLAYER list, left out
// of PLAYERCHOICE) [orig: PlayerInfo_InitProfileSelector @0x56129b;
// UI_InitLANMultiplayerScreen @0x556960].
inline constexpr uint32_t kRecordFlagNoName = 1;

// The campaign table, a byte per mission: +64 + 32 * campaign + mission
// [orig: sub_54D6A0 @0x54d70a].
inline constexpr size_t kCampaignOffset = 64;
inline constexpr size_t kCampaigns = 16;
inline constexpr size_t kCampaignMissions = 32;

// The ten chat macros, 40 bytes each, the first 39 a string
// [orig: PlayerProfile_InitDefaults @0x54bc9a..0x54bcda; copied whole into the
// session's macro table by Game_ApplySessionSettingsToGlobals @0x552081].
inline constexpr size_t kMacroOffset = 576;
inline constexpr size_t kMacroCount = 10;
inline constexpr size_t kMacroBytes = 40;

// The key-binding table: +1804 the count, +1808 up to 190 72-byte entries
// [orig: PlayerProfile_InitDefaults @0x54bb8c (memcpy 0x3570), @0x54bba6].
inline constexpr size_t kBindingCountOffset = 1804;
inline constexpr size_t kBindingOffset = 1808;
inline constexpr size_t kBindingBytes = 72;
inline constexpr size_t kBindingCapacity = 190;  // 0x3570 / 72
inline constexpr size_t kBindingTokenOffset = 38;
inline constexpr size_t kBindingTokenBytes = kBindingBytes - kBindingTokenOffset;  // 34

// One key-binding entry: a catalog row's identity and its eight binding
// fields [orig: KeyBinding_BuildFilteredTable @0x54c2b0 builds them;
// sort_and_copy_weapon_loadout_to_session @0x559d50 writes the live ones back;
// UI_BuildKeyBindingLoadoutTable @0x559e50 and sub_562DF0 @0x562df0 read them].
struct BindingEntry {
	uint16_t id = 0;          // +0, the catalog row's action id
	int32_t index = 0;        // +4, the row's place in the sorted catalog (its id)
	uint32_t flags = 0;       // +8, the row's flag word
	uint32_t modes = 0;       // +12
	uint32_t action_class = 0;  // +16, the Class column
	uint32_t help = 0;        // +20, the row's help text id
	uint16_t primary = 0;     // +24, slot-1 VK
	uint16_t secondary = 0;   // +26, slot-2 VK
	uint16_t primary_mod = 0;   // +28
	uint16_t secondary_mod = 0; // +30
	uint16_t mouse_mask = 0;  // +32
	uint16_t mouse_mod = 0;   // +34
	uint8_t joy_button = 0;   // +36
	uint8_t joy_mod = 0;      // +37
	std::string token;        // +38, the row's config token; the merge's key
};

// One profile record. Fields named for their role carry it; a `word_<offset>`
// is a dword the defaults seed and a session-start copy forwards to a global
// whose meaning is not walked (docs/playerinfo/player-sav-re.md has each one's
// reader).
struct ProfileRecord {
	uint32_t tag = kRecordTag;           // +0
	std::string name;                    // +4
	uint32_t flags = kRecordFlagNoName;  // +52
	int32_t last_campaign = 0;           // +60, the started mission's campaign (-1 none)
	std::array<std::array<uint8_t, kCampaignMissions>, kCampaigns> campaign_complete{};  // +64
	std::array<std::string, kMacroCount> macros;  // +576
	int32_t campaigns_won = 0;           // +1340
	// The single-player session words, the host's game.cfg words' places
	// outside a session [orig: Game_ApplySessionSettingsToGlobals
	// @0x551F15..0x551F75].
	int32_t sp_no_char_abilities = 0;    // +1352 (mp_NoCharAbilities)
	int32_t sp_no_weapon_recoil = 0;     // +1356 (mp_NoWeaponRecoil)
	int32_t sp_no_scope_drift = 0;       // +1360 (mp_NoScopeDrift)
	int32_t sp_no_crosshair_spread = 0;  // +1364 (mp_NoCrossHairSpread)
	int32_t sp_wind = 0;                 // +1368 (mp_wind)
	int32_t sp_gps_icons = 1;            // +1372 (mp_gpsicons)
	int32_t sp_no_drop_weapons = 0;      // +1376 (mp_NoDropWeapons)
	int32_t sp_difficulty = 0;           // +1380 (mp_difficulty)
	int32_t intro_pending = -1;          // +1412, cleared after the intro videos
	int32_t word_1416 = 1;               // +1416
	int32_t mouse_enabled = 1;           // +1420
	int32_t mouse_sensitivity = 128;     // +1424, MOUSE_SENSITIVITY
	int32_t invert_mouse = 0;            // +1428, INVERT_MOUSE
	int32_t joystick_enabled = 0;        // +1432, ENABLE_JOYSTICK
	int32_t invert_joystick = 0;         // +1436, INVERT_JOYSTICK
	int32_t joystick_cap_2 = 0;          // +1440
	int32_t force_feedback = 0;          // +1444, ENABLE_FORCE_FEEDBACK
	int32_t word_1448 = 0;               // +1448
	int32_t word_1452 = 0;               // +1452
	int32_t joystick_cap_4 = 0;          // +1456
	int32_t word_1460 = 0;               // +1460
	int32_t word_1464 = 1;               // +1464
	int32_t word_1468 = 127;             // +1468
	int32_t word_1472 = 0;               // +1472
	int32_t word_1476 = 0;               // +1476
	int32_t word_1480 = 0;               // +1480
	int32_t view_mode = 1;               // +1484
	int32_t word_1488 = 0;               // +1488
	int32_t word_1496 = 0;               // +1496
	int32_t word_1500 = 1;               // +1500
	int32_t word_1504 = 1;               // +1504
	int32_t word_1508 = 1;               // +1508
	int32_t joystick_cap_1 = 0;          // +1512
	int32_t word_1516 = 1;               // +1516
	int32_t word_1520 = 1;               // +1520
	int32_t auto_reload = 1;             // +1524, OPTIONS_AUTORELOAD
	int32_t word_1528 = 0;               // +1528
	std::array<uint8_t, 2> voice{};      // +1532 / +1533, per side (0 = the character's own)
	int32_t auto_medic_off = 0;          // +1660, the INVERSE of OPTIONS_AUTOMEDIC
	std::vector<BindingEntry> bindings;  // +1804 count, +1808 entries
};

struct PlayerSav {
	uint32_t flags = 0;  // header dword 2
	uint32_t extra = 0;  // header dword 3
	std::array<ProfileRecord, kProfileSlots> slots;
};

// The record's dwords, by name and offset, in offset order: the one table the
// reader, the writer and a tool that edits a record by field name share. The
// name is the member's.
struct RecordWord {
	const char *name;
	size_t offset;
	int32_t ProfileRecord::*member;
};
const RecordWord *record_words(size_t *count);
// The word named `name` (exact), or null.
const RecordWord *find_record_word(const std::string &name);

// One record's image to its fields: `data` must hold kPlayerRecordBytes. A
// count past the table's capacity reads the capacity; a negative one reads none.
ProfileRecord read_record(const uint8_t *data);

// The record's image, from scratch (docs/adr/0003): kPlayerRecordBytes bytes.
void write_record(const ProfileRecord &record, uint8_t *out);

// Parse. False on a buffer shorter than the header and the five records, or a
// header other than "FPBC"/"0211" [orig: @0x54f5c6]. The trailer is not read.
bool read(const uint8_t *data, size_t size, PlayerSav &out);

// The file from scratch (docs/adr/0003): always kPlayerSavBytes.
std::vector<uint8_t> write(const PlayerSav &in);

}  // namespace opennova::playersav
