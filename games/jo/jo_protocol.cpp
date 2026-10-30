#include "jo.h"

#include <games/novalogic_2009/generation.h>

namespace opennova::games {

namespace {

// Jointops.exe's protocol: its network init's identity block over the 2009 generation, its
// in-match sheet and its NovaWorld values. Its engine build (1.7.5.7, its build dates) is no
// protocol's: ONE's build fills those slots (Protocol, net/protocol/protocol.h). Every other
// protocol copies this row and overrides it (dfx2_protocol.cpp).
Protocol make() {
	Protocol p;
	// --- Identity: the network init's block over the 2009 generation.
	p.code = "jo";
	p.name = "JO";
	// [orig: CNapiNetwork_Init @0x4ca790, the literal @0x7ca6c8]
	p.pn = "JOINTOPERATIONS";
	// QUuid(0xB074D646, 0x81F9, 0x475F, 0x92,0xDA,0xDE,0xA7,0x24,0x7F,0x14,0x68): QUuid's first
	// three components occupy native little-endian memory, and PG copies those 16 bytes.
	// [orig: the static initializer @0x7937a0 into g_GameProtocolGuid; CNapiNetwork_Init
	//  @0x4ca7a4..0x4ca7af -> NapiNPVarBlock_Copy(proto+0x11C)]
	p.guid = std::array<uint8_t, 16>{ 0x46, 0xD6, 0x74, 0xB0, 0xF9, 0x81, 0x5F, 0x47, 0x92, 0xDA,
		0xDE, 0xA7, 0x24, 0x7F, 0x14, 0x68 };
	// [orig: CNapiNetwork_Init @0x4ca7c2, the literal @0x7ca6b4]
	p.pv1 = "0.0.0 1/12/2004 EM";
	// PV3's bracket pair: the addresses of the "3" literal @0x7c9730 (8165168) and the "2345"
	// literal @0x7c9714 (8165140), the version pair's own strings.
	// [orig: CNapiNetwork_Init @0x4ca83d..0x4ca85b]
	p.pv3_tags = { 0x7C9730u, 0x7C9714u };
	p.generation = &novalogic_2009();

	// --- The in-match sheet, by the message each knob shapes (net/protocol/protocol.h
	// kProtocolDialect), the baseline docs/titles/dfx2-vs-jo-re.md §8 reads every row against.
	// Each value is ProtocolInMatch's default, set here as the sheet.
	ProtocolInMatch &m = p.in_match.emplace();
	// S2C 0x0A (DFX2-1): the objective body's four values, no checkpoint after them.
	// [orig: NetPacket_WritePlayerState @0x4ffc2d..0x4ffc99; NapiNPClientMsg_0x00A
	//  @0x430361..0x4303d0]
	m.s2c_0a.objective_checkpoint = false;
	// S2C 0x0F (DFX2-2): the flags byte's bit 0 only with spawn zones.
	// [orig: NetPacket_WriteWorldStateLoad0x0F @0x502da7..0x502db0]
	m.s2c_0f.spawn_zone_flag_always = false;
	// S2C 0x0F (DFX2-34): the joiner's deploy screen cleared, then raised by bit 0 with no death
	// screen. [orig: NapiNPClientMsg_0x00F @0x42e2d8, @0x42e2ed..0x42e2f8]
	m.s2c_0f.deploy_screen_always = false;
	// S2C 0x14 (DFX2-14): the squad tag bare. [orig: NapiNPServer_HandleChatMessage @0x513930]
	m.s2c_14.chat_squad_tag_wrapped = false;
	// S2C 0x25 (DFX2-17): a joiner's reset zeroes the end-round linger, so its next frame leaves
	// the board. [orig: NapiNPClientMsg_GameReset @0x42281f]
	m.s2c_25.end_round_linger_ticks = 0;
	// S2C 0x5A (DFX2-35): a joiner's apply selects the weapon the body holds.
	// [orig: NapiNPClientMsg_HandleWeaponLoadoutSync @0x4296bc..0x4296d4]
	m.s2c_5a.select_last_primary_row = false;
	// S2C 0x64 (DFX2-18): the rules word as built.
	// [orig: CNapiGameSession_InitRandomSeedOrRequest @0x51e94b..0x51e957]
	m.s2c_64.rules_word_bits = 0;
	// S2C 0x66 (DFX2-10): armory-only 2, the mission's 3.
	// [orig: NetPacket_SerializeWeaponRestrictionTable @0x5102e0;
	//  NapiNPClientMsg_HandleWeaponRestrictions @0x42d51c; CAdminServer_HandleWeaponCommand
	//  @0x4046ba, @0x40482e]
	m.s2c_66.armory_only_availability = 2;
	// C2S 0x2F (DFX2-12): the profile's class. [orig: NetPacket_SendLoadoutSubmit @0x42ceae]
	m.c2s_2f.loadout_class_fixed = false;
	// The slot state (DFX2-11): the join's byte 1, 3 in the pre-round, 0x10 with spawn zones; the
	// deploy's 1 or 3.
	// [orig: Server_OnPlayerJoin @0x51a6d3..0x51a6f2; Server_DeployPlayer @0x517803..0x517812]
	m.slot_state.join_state_fixed = false;
	// The person index (D-SIM-52): one byte names a person by its pool-0 index, so the wire names
	// 256 at most. [orig: NetPacket_SerializePlayerSync0x46 @0x505E80 - Pool_GetIndexFromPtr
	//  @0x505F40, the byte store @0x505F58]
	m.person_index.pool0_wire_capacity = 256;
	// C2S 0x00 (DFX2-13): no CD-key leg in the join check.
	// [orig: Server_ValidatePlayerJoinRequest @0x5122a9..0x5122f0]
	p.cd_key = nullptr;

	// --- NovaWorld.
	// [orig: Lobby_UpdateServerInfo @0x4ff3ea / @0x4ff3ff; Mission_DeobfuscateDescription
	//  @0x4cdb9f / @0x4cdbb6 - the literals @0x7c9730 / @0x7c9714]
	p.ver1 = "3";
	p.ver2 = "2345";
	// [orig: Mission_DeobfuscateDescription @0x4cdbfe]
	p.product_code = "jop";
	// [orig: CNapiGateManager_InitDefaults @0x4d14b6, the literal @0x7cc35c]
	p.gate_tag = "jop:cus2";
	// [orig: Mission_LoadEncryptedConfig @0x4cdd65, the literal @0x7cbb44]
	p.gate_tag_keys = "jop:2:oyez";
	// The host lists end at LAN. [orig: CNapiGameSession_BuildHostVarLists @0x4d0b50]
	p.host_category_var = false;
	// NWPSSK's fourth dword takes (serial ^ 0x0B24B390) >> 3.
	// [orig: CDKey_GenerateHardwareFingerprint @0x4a4a00 - `xor eax, 0B24B390h` @0x4a4b1f,
	//  `shr eax, 3` @0x4a4b24, into the dword @0x4a4b2a]
	p.nwpssk_mix = FingerprintSerialMix{ 0x0B24B390u, /*shift_first=*/false };
	// jop_2_login.mnx's submit is ACCEPT: no LOGIN control.
	p.sign_in_submit = "ACCEPT";
	return p;
}

} // namespace

const Protocol &jo_protocol() {
	static const Protocol protocol = make();
	return protocol;
}

} // namespace opennova::games
