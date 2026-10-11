#pragma once

// Retail's in-memory ammo.def table, rebuilt byte for byte as the mission
// load leaves it, for the anti-cheat challenge's per-row CRC (S2C 0x31 ->
// C2S 0x21). The table is the image retail CRCs, so the build is a
// structural translation of the parse that writes it: the count pass that
// sizes it, the 276-byte rows the parse fills in file order with their
// allocation defaults, the effects tables, and the inherit pass, over the
// retail tokenizer (base/io/ascii_config.h) whose stale slots carry from the
// count pass into the parse. Two inputs are not ammo.def's: the item list the
// tracer ids resolve against (items.def, in file order) and the sound banks
// the whiz radius reads (the six global slots, loaded right before ammo.def).
// [orig: AmmoDef_LoadAll @0x40B0B0; AmmoDef_ParseProperty @0x40A2D0;
//  NetPacket_WriteEntityCRCChecksum @0x42B020]

#include <base/io/ascii_config.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::audio {
class SoundSetIndex;
}

namespace opennova::inmatch {

// One g_AmmoDefTable row [orig: the 276-byte stride of AmmoDef_AllocateSlot
// @0x409A46 and NetPacket_WriteEntityCRCChecksum @0x42B045].
inline constexpr size_t kAmmoRecordSize = 276;

// One g_ItemDefs row as the ammo parse reads it: the primary name (ItemDef+0)
// and the type id (ItemDef+0x50).
struct IntegrityItemRow {
	std::string name;
	int32_t id = 0;
};

// The item list items.def leaves at boot, one row per `begin` line in file
// order: the name cut to 46 characters, the id the block's `id` line less
// 100000 (0 with none). [orig: ItemDefs_LoadAndValidate @0x4A1DA0 ->
// ItemDef_ParseProperty @0x49EB00 (the begin arm @0x49EBA8 ->
// ItemDef_AllocateWithDefaults @0x49E3B0, which memsets the row; the id arm
// @0x49EC54)]
std::vector<IntegrityItemRow> read_integrity_item_rows(const uint8_t *data, size_t size);

// The table as AmmoDef_LoadAll leaves it.
struct AmmoRecordTable {
	// g_AmmoDefCapacity: every `ammo` line the count pass saw, the rows the
	// challenge can name [orig: AmmoDef_CountCallback @0x409E70].
	int32_t capacity = 0;
	// g_AmmoDefCount: the rows the parse allocated (fewer than the capacity
	// when a nested `ammo` aborted the parse; the rest stay all zero).
	int32_t count = 0;
	// capacity * kAmmoRecordSize bytes.
	std::vector<uint8_t> bytes;

	// The first row whose name stricmp's `name`, 0 on a miss (the first row
	// itself is therefore never found) [orig: AmmoDef_LookupByName @0x409870].
	int32_t lookup_by_name(const char *name) const;
	// The challenge's CRC of row `index`: the row with its six pointer and
	// handle dwords (+0x40, +0x44, +0x48, +0x4C, +0x68, +0x70) zeroed, through
	// CRC-32/MPEG-2. False when `index` is out of [0, capacity), where retail
	// replies 0. [orig: NetPacket_WriteEntityCRCChecksum @0x42B020, the range
	// gate @0x42B038, the zeroing @0x42B072..0x42B0A6, the CRC @0x42B0B9]
	bool row_crc(int32_t index, uint32_t &crc) const;
};

// Build the table from ammo.def's bytes. `items` is the item list
// (read_integrity_item_rows); `sounds` the six global banks in slot order,
// null for none loaded. `tokens` carries the tokenizer's state through the
// two walks and out, for the next file the load tokenizes (weapon.def).
AmmoRecordTable build_ammo_record_table(const uint8_t *data, size_t size,
		const std::vector<IntegrityItemRow> &items, const audio::SoundSetIndex *sounds,
		io::ConfigTokens &tokens);

} // namespace opennova::inmatch
