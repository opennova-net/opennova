#pragma once

// The live witnesses of the anti-cheat challenge sources, kept as the oracles
// the sources computed from definitions (runtime/inmatch/integrity_sources.h)
// must reproduce. Each ammo table is every row of g_AmmoDefTable read from one
// live retail process (scripts/net/read_integrity_profile.py: the 276-byte rows
// with NetPacket_WriteEntityCRCChecksum @0x42B020's six dwords zeroed, through
// CRC-32/MPEG-2); each table CRC is the retail client's C2S 0x20 reply to a
// stock host's {FF,00,00} challenge [orig: WeaponSlotDef_ComputeSanitizedChecksum
// @0x53F8F0].
//   revx02: one 1.7.5.7 process (2026-08-01); 140 live weapon rows.
//   jox01: the pinned JO:CA reference (Jointops.exe SHA-256
//     9a1035440a53af2057ce0995ac42dced840d3b9fd53c04dc86041a962b84fe57), one
//     stock host per mission, identical on 01TR (COOP) and ASH_I5A (AAS); the
//     table CRC and row 0x16 (0xB4D3A790) are the retail client's replies in
//     fixtures/novaworld/run_20260426_120859.

#include <cstddef>
#include <cstdint>

namespace integrity_oracle {

inline constexpr uint32_t kRevx02WeaponTableCrc = 0x024F56F2u;
inline constexpr uint32_t kRevx02AmmoRowCrcs[] = {
	0x445867A1u,
	0x8B0772BAu,
	0x6C9BDFCDu,
	0x75BB7700u,
	0x0B28C40Au,
	0xC610F3DEu,
	0xCF559FD7u,
	0xBB1373C6u,
	0xEBE48ACFu,
	0x24FB7399u,
	0xF27F6797u,
	0x1AC46099u,
	0x30AB4F08u,
	0x0E1EB4FAu,
	0xD91ED5ACu,
	0x1BF8CD85u,
	0x0E9E38B7u,
	0x0D7D0151u,
	0xA76C4145u,
	0x0907FD52u,
	0x0A2E608Bu,
	0x069859DBu,
	0x80D44C5Fu,
	0x066C162Au,
	0x2D087374u,
	0xD7550DE7u,
	0x616CD2EEu,
	0x960F2BE7u,
	0x7DC30FBCu,
	0xA911EACCu,
	0x6CFA5CA2u,
	0x834962CAu,
	0xAF6E1207u,
	0x0BB538A6u,
	0x20D23CE6u,
	0x9CFE1B0Au,
	0x7CB15ACBu,
	0xF5487884u,
	0x2721802Eu,
	0x47868C2Fu,
	0xB5F05C38u,
	0x3A90A0D2u,
	0xA1752A14u,
	0x977120B8u,
	0xC9710791u,
	0xBB351358u,
	0x25A2A217u,
	0xAA0EEDEBu,
	0xABBE873Eu,
	0x14DD8C73u,
	0x78F09C65u,
	0x6845C662u,
	0xDC1DFFDCu,
	0x4115DBDDu,
	0xF4548547u,
	0x2F0EF42Bu,
	0xA71B8DFFu,
	0x75AB462Eu,
	0x6980587Cu,
	0x32CFC038u,
	0xF2F438D1u,
	0x6D375DDEu,
	0x8546DFF2u,
	0x7FB6462Eu,
	0x29EC86DFu,
	0xAD8B73C1u,
	0x8FA6B9C6u,
	0x86148E15u,
	0xC945578Cu,
	0x052BE3D6u,
	0x7C85F4A9u,
	0x588CBC90u,
	0xB36721F7u,
	0x118E8882u,
	0x09B729D6u,
	0x83264FF9u,
	0xED19C9B4u,
	0x9E25C24Cu,
	0xEE79E935u,
	0xDA42D5FCu,
	0xE03C3A3Eu,
	0x6013CA1Fu,
	0x413683BEu,
	0x6513B15Du,
	0xF8E7B1C7u,
	0x80DA708Cu,
	0xE61FAEEFu,
	0x1694A0C3u,
	0xF53BB2B2u,
	0xBD848BE6u,
	0x321AE4B8u,
	0x6F79AA4Eu,
	0x5AFB0A79u,
	0xF88AB3FFu,
	0xD19366D9u,
	0xDA3ECB9Bu,
	0x3CDF410Fu,
	0xC8F71020u,
	0xC259B3E0u,
	0x2EF921B5u,
	0x191E915Fu,
	0x682FD8EEu,
};
static_assert(sizeof(kRevx02AmmoRowCrcs) / sizeof(kRevx02AmmoRowCrcs[0]) == 102);

inline constexpr uint32_t kJox01WeaponTableCrc = 0x47724F02u;
inline constexpr uint32_t kJox01AmmoRowCrcs[] = {
	0x445867A1u,
	0x687AE92Bu,
	0x019C3B58u,
	0x5676601Du,
	0xFA6767C1u,
	0x2A452975u,
	0x7AB2D07Cu,
	0x5B48EF3Fu,
	0xF27F6797u,
	0xB3B9B4BCu,
	0xA1FD15BBu,
	0x0E1EB4FAu,
	0xBE45DED0u,
	0x281B1A06u,
	0x21F86840u,
	0x830F5936u,
	0xC1F52ADCu,
	0xECC401BCu,
	0xFC3BF8B8u,
	0x7FFE62B3u,
	0x88E37ECDu,
	0x5B292E86u,
	0xB4D3A790u,
	0xCA8FBD0Au,
	0x6422E3A9u,
	0x5352AE16u,
	0xE0E18838u,
	0x83C74B1Eu,
	0x4DADABAEu,
	0x2EED8BAFu,
	0x7805AE61u,
	0x5F5FDDA1u,
	0x09A93C7Eu,
	0xCFA430FEu,
	0xD9F346B1u,
	0x8E8DFBC9u,
	0xEC81E16Au,
	0x82966D39u,
	0xD76829B6u,
	0x177C5656u,
	0xB52591EDu,
	0xB21FAFFEu,
	0x5D513E9Du,
	0xAE39693Fu,
	0x6502DFF4u,
	0xBE58AE98u,
	0x939B2995u,
	0x28BFF2AEu,
	0xA9D33A25u,
	0xD48EA7C7u,
	0xF5426AFEu,
	0x6D375DDEu,
	0x14108541u,
	0xEEE01C9Du,
	0x57DCDD0Au,
	0x552D4513u,
	0x120A72D5u,
	0x947DB965u,
	0x7C85F4A9u,
	0x61B1C8DEu,
	0x22317B44u,
	0x6174E03Fu,
	0x066EB819u,
	0x5120D23Bu,
	0x7B83B778u,
	0x08BFBC80u,
	0x69467357u,
	0xF14590ACu,
	0xD060D90Du,
	0xF445EBEEu,
	0x69B1EB74u,
	0x182165B2u,
	0x0AD8FE07u,
	0x3CC202AAu,
	0xA6DC2359u,
};
static_assert(sizeof(kJox01AmmoRowCrcs) / sizeof(kJox01AmmoRowCrcs[0]) == 75);

// FNV-1a 64 over every `[u8 row][u32 crc LE]` tuple, the compact pin of a
// whole table.
inline constexpr uint64_t kRevx02RowDigest = 0xCAED42DE09D8AD38ull;
inline constexpr uint64_t kJox01RowDigest = 0x7B7F326C906F274Bull;

inline uint64_t row_digest(const uint32_t *crcs, size_t count) {
	uint64_t digest = 14695981039346656037ull;
	for (size_t row = 0; row < count; ++row) {
		const uint8_t bytes[5] = { static_cast<uint8_t>(row), static_cast<uint8_t>(crcs[row]),
			static_cast<uint8_t>(crcs[row] >> 8), static_cast<uint8_t>(crcs[row] >> 16),
			static_cast<uint8_t>(crcs[row] >> 24) };
		for (const uint8_t b : bytes) {
			digest ^= b;
			digest *= 1099511628211ull;
		}
	}
	return digest;
}

} // namespace integrity_oracle
