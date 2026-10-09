// DBF (DLG0) dialog-bank parser for NovaLogic games.
//
// Reverse-engineered from sndc.exe (Sound Script Compiler); ported from the
// pre-repo prototype's engine/formats/dbf, then grilled against the engine reader
// in Jointops.exe (2026-06-09): header/record layout witnessed by
// DialogManager_LoadFromFile @ 0x44e650 (28-byte header, 52-byte group records
// each followed by its 68-byte line records). The engine loads the mission's
// co-named .dbf from DialogSystem_Init @ 0x5275e0 (mission base name + ".dbf"),
// then co-loads "<base>.lwf" (falling back to "<base>.pwf") as the dialog sound
// bank @ 0x44e7d4. A mission's .DBF maps a dialog id (group_name, e.g. "dlg001")
// to one or more lines, each naming a wave of that bank (def_id_name, e.g.
// "Z00gR100": a single, found by name [orig: Dialog_LoadAudioClip @ 0x44dcf7 ->
// SoundBank_FindEntryByName @ 0x75bba0], never a set). Mission PlayWavList actions
// reference a dialog id by its number. See docs/audio/lwf-dbf-sound-re.md.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova {
namespace dbf {

// Magic number "DLG0" in little-endian.
constexpr uint32_t kMagic = 0x30474C44;  // 'DLG0'

// The bytes the format keeps a name in, its terminator among them: a dialog's
// 24 [orig: the 52-byte record's name at +4, the strcmp of Dialog_PlayByName
// @ 0x44da8b], a line's wave 24 and its sequence 24 [orig: the 68-byte line's
// +4 and +28, Dialog_LoadAudioClip @ 0x44dcf7 / @ 0x44ddec].
inline constexpr size_t kNameBytes = 24;

// 28-byte file header [orig: DialogManager_LoadFromFile reads 0x1C @ 0x44e699;
// magic check 'DLG0' @ 0x44e6f7; version/header_size are never checked].
struct Header {
	uint32_t magic;           // 0x00: 'DLG0'
	uint32_t version;         // 0x04: observed 0x100 (unchecked by the engine)
	uint32_t header_size;     // 0x08: always 28 (unchecked by the engine)
	uint32_t id_def_count;    // 0x0C: count of 32-byte id-def records ("DlgMgr IDDEFS" @ 0x44e6f7); 0 in JO missions
	uint32_t id_defs_offset;  // 0x10: offset to the id-def table [orig: seek @ 0x44e702]
	uint32_t entry_count;     // 0x14: number of dialog groups
	uint32_t entries_offset;  // 0x18: offset to the first group [orig: seek @ 0x44e723]
};

static_assert(sizeof(Header) == 28, "Header must be 28 bytes");

// 52-byte group record (raw on-disk).
struct RawGroupRecord {
	uint32_t record_size;        // 0x00: always 52
	char group_name[kNameBytes]; // 0x04: DLG_ID (null-terminated)
	uint32_t line_count;         // 0x1C
	uint32_t idlist_count;       // 0x20
	uint8_t def_id_indices[16];  // 0x24
};

static_assert(sizeof(RawGroupRecord) == 52, "RawGroupRecord must be 52 bytes");

// 68-byte line record (raw on-disk).
struct RawLineRecord {
	uint32_t line_flags;   // 0x00
	char def_id_name[kNameBytes]; // 0x04: the wave of the dialog bank's sounds it plays, by name (null-terminated)
	char sequence[kNameBytes];    // 0x1C: the subtitle entry: digits after the last '_' index the mission text ("_00003"; "##" none)
	uint8_t def_id_index;  // 0x34
	uint8_t delay;         // 0x35: DELAY (low byte)
	uint16_t padding;      // 0x36
	uint32_t param;        // 0x38
	uint32_t resd0;        // 0x3C
	uint32_t resd1;        // 0x40
};

static_assert(sizeof(RawLineRecord) == 68, "RawLineRecord must be 68 bytes");

struct Line {
	std::string def_id_name;
	std::string sequence;
	uint32_t line_flags = 0;
	uint8_t def_id_index = 0xFF;
	uint8_t delay = 0;
	uint32_t param = 0;
	uint32_t resd0 = 0;
	uint32_t resd1 = 0;
};

struct Group {
	std::string group_name;
	uint32_t idlist_count = 0;
	std::vector<uint8_t> def_id_indices;
	std::vector<Line> lines;
};

struct File {
	Header header{};
	std::vector<Group> groups;
};

bool parse_dbf_memory(const uint8_t *data, size_t size, File &out, std::string &error);

// Encode / write (used by the round-trip test).
bool encode_dbf(const File &file, std::vector<uint8_t> &out, std::string &error);

// Find group by name (case-insensitive); nullptr if not found.
const Group *find_group(const File &file, const std::string &name);

}  // namespace dbf
}  // namespace opennova
