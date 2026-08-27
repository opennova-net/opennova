#include <net/npruntime/integrity_challenge_profile.h>

namespace opennova::np {
namespace {

// [orig: NetPacket_WriteEntityChecksum @0x42AFB0 ->
// WeaponSlotDef_ComputeSanitizedChecksum @0x53F8F0;
// NetPacket_WriteEntityCRCChecksum @0x42B020]
// Independently reproduced from one live retail 1.7.5.7/revx02 process using
// ReadProcessMemory over the complete 102-row table. Each 276-byte row was
// cloned, then little-endian dwords at 0x40/0x44/0x48/0x4C/0x68/0x70 were
// zeroed exactly as NetPacket_WriteEntityCRCChecksum @0x42B020 does before
// CRC_ComputeCustomTable @0x53C820 (CRC32/MPEG-2). Cross-check anchors:
// row 0x18 => 0x2D087374; row 0x1A => 0x616CD2EE.
constexpr IntegrityCrcEntry kRetailRevx02AmmoCrcs[] = {
		{0x00, 0x445867A1u},
		{0x01, 0x8B0772BAu},
		{0x02, 0x6C9BDFCDu},
		{0x03, 0x75BB7700u},
		{0x04, 0x0B28C40Au},
		{0x05, 0xC610F3DEu},
		{0x06, 0xCF559FD7u},
		{0x07, 0xBB1373C6u},
		{0x08, 0xEBE48ACFu},
		{0x09, 0x24FB7399u},
		{0x0A, 0xF27F6797u},
		{0x0B, 0x1AC46099u},
		{0x0C, 0x30AB4F08u},
		{0x0D, 0x0E1EB4FAu},
		{0x0E, 0xD91ED5ACu},
		{0x0F, 0x1BF8CD85u},
		{0x10, 0x0E9E38B7u},
		{0x11, 0x0D7D0151u},
		{0x12, 0xA76C4145u},
		{0x13, 0x0907FD52u},
		{0x14, 0x0A2E608Bu},
		{0x15, 0x069859DBu},
		{0x16, 0x80D44C5Fu},
		{0x17, 0x066C162Au},
		{0x18, 0x2D087374u},
		{0x19, 0xD7550DE7u},
		{0x1A, 0x616CD2EEu},
		{0x1B, 0x960F2BE7u},
		{0x1C, 0x7DC30FBCu},
		{0x1D, 0xA911EACCu},
		{0x1E, 0x6CFA5CA2u},
		{0x1F, 0x834962CAu},
		{0x20, 0xAF6E1207u},
		{0x21, 0x0BB538A6u},
		{0x22, 0x20D23CE6u},
		{0x23, 0x9CFE1B0Au},
		{0x24, 0x7CB15ACBu},
		{0x25, 0xF5487884u},
		{0x26, 0x2721802Eu},
		{0x27, 0x47868C2Fu},
		{0x28, 0xB5F05C38u},
		{0x29, 0x3A90A0D2u},
		{0x2A, 0xA1752A14u},
		{0x2B, 0x977120B8u},
		{0x2C, 0xC9710791u},
		{0x2D, 0xBB351358u},
		{0x2E, 0x25A2A217u},
		{0x2F, 0xAA0EEDEBu},
		{0x30, 0xABBE873Eu},
		{0x31, 0x14DD8C73u},
		{0x32, 0x78F09C65u},
		{0x33, 0x6845C662u},
		{0x34, 0xDC1DFFDCu},
		{0x35, 0x4115DBDDu},
		{0x36, 0xF4548547u},
		{0x37, 0x2F0EF42Bu},
		{0x38, 0xA71B8DFFu},
		{0x39, 0x75AB462Eu},
		{0x3A, 0x6980587Cu},
		{0x3B, 0x32CFC038u},
		{0x3C, 0xF2F438D1u},
		{0x3D, 0x6D375DDEu},
		{0x3E, 0x8546DFF2u},
		{0x3F, 0x7FB6462Eu},
		{0x40, 0x29EC86DFu},
		{0x41, 0xAD8B73C1u},
		{0x42, 0x8FA6B9C6u},
		{0x43, 0x86148E15u},
		{0x44, 0xC945578Cu},
		{0x45, 0x052BE3D6u},
		{0x46, 0x7C85F4A9u},
		{0x47, 0x588CBC90u},
		{0x48, 0xB36721F7u},
		{0x49, 0x118E8882u},
		{0x4A, 0x09B729D6u},
		{0x4B, 0x83264FF9u},
		{0x4C, 0xED19C9B4u},
		{0x4D, 0x9E25C24Cu},
		{0x4E, 0xEE79E935u},
		{0x4F, 0xDA42D5FCu},
		{0x50, 0xE03C3A3Eu},
		{0x51, 0x6013CA1Fu},
		{0x52, 0x413683BEu},
		{0x53, 0x6513B15Du},
		{0x54, 0xF8E7B1C7u},
		{0x55, 0x80DA708Cu},
		{0x56, 0xE61FAEEFu},
		{0x57, 0x1694A0C3u},
		{0x58, 0xF53BB2B2u},
		{0x59, 0xBD848BE6u},
		{0x5A, 0x321AE4B8u},
		{0x5B, 0x6F79AA4Eu},
		{0x5C, 0x5AFB0A79u},
		{0x5D, 0xF88AB3FFu},
		{0x5E, 0xD19366D9u},
		{0x5F, 0xDA3ECB9Bu},
		{0x60, 0x3CDF410Fu},
		{0x61, 0xC8F71020u},
		{0x62, 0xC259B3E0u},
		{0x63, 0x2EF921B5u},
		{0x64, 0x191E915Fu},
		{0x65, 0x682FD8EEu},
};

static_assert(sizeof(kRetailRevx02AmmoCrcs) /
		sizeof(kRetailRevx02AmmoCrcs[0]) == 102);

// WeaponSlotDef_ComputeSanitizedChecksum over all 140 active 1120-byte ADM
// records in the same process => 0x024F56F2. Individual ADM rows were not
// witnessed, so the profile intentionally exposes none.
constexpr IntegrityChallengeProfile kRetailRevx02Profile = {
		kRetailRevx02IntegrityProfileId,
		0x024F56F2u,
		102,
		nullptr,
		0,
		kRetailRevx02AmmoCrcs,
		sizeof(kRetailRevx02AmmoCrcs) / sizeof(kRetailRevx02AmmoCrcs[0]),
};

bool find_crc(const IntegrityCrcEntry *entries, std::size_t count,
		uint8_t index, uint32_t &crc_out) {
	for (std::size_t i = 0; i < count; ++i) {
		if (entries[i].index != index) continue;
		crc_out = entries[i].crc;
		return true;
	}
	return false;
}

} // namespace

bool IntegrityChallengeProfile::find_animation_definition_crc(
		uint8_t index, uint32_t &crc_out) const {
	return find_crc(animation_definition_crcs,
			animation_definition_crc_count, index, crc_out);
}

bool IntegrityChallengeProfile::find_ammo_definition_crc(
		uint8_t index, uint32_t &crc_out) const {
	return find_crc(ammo_definition_crcs, ammo_definition_crc_count,
			index, crc_out);
}

const IntegrityChallengeProfile *find_integrity_challenge_profile(
		std::string_view id) {
	return id == kRetailRevx02Profile.id ? &kRetailRevx02Profile : nullptr;
}

} // namespace opennova::np
