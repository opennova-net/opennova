#pragma once

// Retail's in-memory weapon.def table (g_AdmDefs: the IDB's "AdmDef" names
// the weapon.def records, unrelated to the .adm animation format), rebuilt
// byte for byte as the mission load leaves it, for the anti-cheat challenge's
// whole-table CRC (S2C 0x30 with id 0xFF -> C2S 0x20). The build is a
// structural translation of the load that writes it: AnimDef_InitAll's
// defaults and its "null" entry, the weapon.def parse into 1120-byte records
// over the retail tokenizer, each block's action bind (whose `auto` delays
// read the weapon's .adm duration rings in file order), and the post-parse
// fire-cycle sum. Bytes the sanitizer overwrites (pointers, handles, the
// pos/tpos floats, the elevation simulation) are left as the defaults; the
// rest come from weapon.def, the ammo table (round_type), the mounted files
// (the image-gated keys) and the .adm/.bad clips.
// [orig: Game_StartMission @0x5254B3 -> AnimDef_InitAll @0x5435C0, @0x5254BD ->
//  WeaponDefs_LoadFile @0x5450A0; WeaponSlotDef_ComputeSanitizedChecksum
//  @0x53F8F0]

#include <base/io/ascii_config.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace opennova::assets {
class AssetStore;
}
namespace opennova::mission {
struct BootFileSource;
}

namespace opennova::inmatch {

struct AmmoRecordTable;

// One g_AdmDefs row and the table's rows [orig: the g_AdmDefs stride 0x460,
// AdmDef_FindFreeSlot's bound @0x53FC6B].
inline constexpr size_t kWeaponRecordSize = 1120;
inline constexpr int kWeaponRecordCapacity = 255;

// The table as WeaponDefs_LoadFile leaves it.
struct WeaponRecordTable {
	// kWeaponRecordCapacity * kWeaponRecordSize bytes.
	std::vector<uint8_t> bytes;
	// False when the load put a value in a CRC'd byte that no definition
	// determines (a fifth `sights` row writes a texture pointer over the row
	// count; a weapon left open by an aborted parse reads an unbound action),
	// so no computed reply can match retail's.
	bool computable = true;

	// The rows the whole-table CRC covers: from row 0 up to the first whose
	// name is empty [orig: @0x53F920].
	int32_t live_count() const;
	// The sanitized whole-table CRC: each live row with its volatile bytes
	// overwritten by fixed values and renderfov reduced to (trunc + 1) >> 1,
	// through CRC-32/MPEG-2; 0 with no live row [orig:
	// WeaponSlotDef_ComputeSanitizedChecksum @0x53F8F0].
	uint32_t sanitized_crc() const;
};

// Build the table from weapon.def's bytes. `ammo` is the mission's ammo table
// (round_type resolves over it); `files` answers the image-gated keys' file
// probes and reads each .adm the action bind loads (whose lines pass through
// the shared tokenizer); `assets` serves the .adm clips' durations. `tokens`
// enters with the state ammo.def's parse left.
WeaponRecordTable build_weapon_record_table(const uint8_t *data, size_t size,
		const AmmoRecordTable &ammo, const mission::BootFileSource &files,
		const assets::AssetStore *assets, io::ConfigTokens &tokens);

} // namespace opennova::inmatch
