#pragma once

// The two anti-cheat challenge sources, computed the way retail computes them:
// from the peer's own loaded definitions, every mission. A retail client
// answers S2C 0x30 / 0x31 from its in-memory weapon.def and ammo.def tables
// and a retail host validates the C2S 0x20 / 0x21 replies against its own, so
// both roles here derive the same CRCs from the mounted corpus the mission
// loads (integrity_ammo_image.h, integrity_weapon_image.h). A leg whose
// inputs are missing, or whose table holds a value no definition determines,
// stays unknown, and an unknown leg is silence: a joiner never answers it
// and a host never punts on it, since a guessed value can only mismatch.
// [orig: NapiNPClientMsg_HandleChecksumRequest @0x431170 ->
//  NetPacket_WriteEntityChecksum @0x42AFB0; NapiNPClientMsg_0x031 @0x4311E0 ->
//  NetPacket_WriteEntityCRCChecksum @0x42B020; Server_ValidateWeaponCRC
//  @0x501F70; NapiNPServerMsg_HandleAntiCheatCRCCheck @0x502050]

#include <cstdint>
#include <vector>

namespace opennova::assets {
class AssetStore;
}
namespace opennova::mission {
struct BootFileSource;
}

namespace opennova::inmatch {

struct IntegritySources {
	// The whole weapon.def table's sanitized CRC (S2C 0x30 with id 0xFF)
	// [orig: WeaponSlotDef_ComputeSanitizedChecksum @0x53F8F0].
	bool weapon_table_known = false;
	uint32_t weapon_table_crc = 0;
	// The table's named rows, which run from row 0 (S2C 0x30 with any other id
	// asks for one row: a named row's CRC hashes process pointers, a row
	// past them has no name and answers 0).
	int32_t weapon_named_rows = 0;
	// One CRC per ammo.def row, g_AmmoDefCapacity of them (S2C 0x31)
	// [orig: NetPacket_WriteEntityCRCChecksum @0x42B020].
	bool ammo_known = false;
	std::vector<uint32_t> ammo_row_crcs;

	// The ammo leg for a challenged index, which both ends read as a signed
	// char: Silent with no ammo source, OutOfRange outside [0, capacity)
	// (the client's reply then carries a zero CRC and the host skips the
	// check), else Crc with the row's CRC [orig: @0x42B038 (the client),
	// @0x5020CA (the host)].
	enum class AmmoRow { Silent, OutOfRange, Crc };
	AmmoRow ammo_row(uint8_t wire_index, uint32_t &crc) const;

	// The one-row weapon CRC for an id other than 0xFF: true with 0 for a row
	// with no name when the table is known; false (silence) for a named row,
	// whose image holds texture, sound and text pointers no definition
	// determines [orig: AnimDef_ComputeChecksum @0x541EF0, the no-name arm
	// @0x541F30].
	bool weapon_row_crc(uint8_t id, uint32_t &crc) const;
};

// Retail's whole-WeaponSlotDef reply has one received-value sentinel: id 0xFF
// with checksum 42 succeeds before the source/key comparison
// [orig: Server_ValidateWeaponCRC @0x501F70, the 42 arm @0x501FD4].
constexpr bool weapon_integrity_reply_matches(
		uint8_t id, uint32_t source_crc, uint32_t salt, uint32_t received_crc) noexcept {
	return (id == 0xFFu && received_crc == 42u) || (source_crc ^ salt) == received_crc;
}

// Both sources over the mission's mounted files, as Game_StartMission loads
// them: items.def's item list, the six global sound banks, ammo.def through
// its count and parse passes, then weapon.def over that ammo table with the
// .adm/.bad clips `assets` serves. The ammo leg needs ammo.def and items.def;
// the weapon leg needs those, weapon.def and an asset store.
// [orig: Game_StartMission @0x524360 — the banks @0x525448..0x525461,
//  AmmoDef_LoadAll @0x525495, AnimDef_InitAll @0x5254B3, WeaponDefs_LoadFile
//  @0x5254BD; ItemDefs_LoadAndValidate @0x4A1DA0 at boot]
IntegritySources compute_integrity_sources(
		const mission::BootFileSource &files, const assets::AssetStore *assets);

} // namespace opennova::inmatch
