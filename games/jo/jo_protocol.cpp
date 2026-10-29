#include "jo.h"

#include <games/novalogic_2009/generation.h>

namespace opennova::games {

namespace {

// Jointops.exe's protocol: its network init's identity block over the 2009 generation, its
// in-match branches, its NovaWorld values and its engine build.
Protocol make() {
	Protocol p;
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
	// The in-match branches the DFX2 catalog reads against (docs/titles/dfx2-vs-jo-re.md §8):
	// the 0x0A objective body's four values [orig: NetPacket_WritePlayerState @0x4ffc2d..0x4ffc99;
	// NapiNPClientMsg_0x00A @0x430361..0x4303d0]; the 0x0F flags bit 0 only with spawn zones
	// [orig: NetPacket_WriteWorldStateLoad0x0F @0x502da7..0x502db0]; armory-only 2 [orig:
	// NetPacket_SerializeWeaponRestrictionTable @0x5102e0; NapiNPClientMsg_HandleWeaponRestrictions
	// @0x42d51c; CAdminServer_HandleWeaponCommand @0x4046ba, @0x40482e]; the join's state byte 1,
	// 3 in the pre-round, 0x10 with spawn zones, the deploy's 1 or 3 [orig: Server_OnPlayerJoin
	// @0x51a6d3..0x51a6f2; Server_DeployPlayer @0x517803..0x517812]; the profile's 0x2F class
	// [orig: NetPacket_SendLoadoutSubmit @0x42ceae]; no CD-key leg in the join check [orig:
	// Server_ValidatePlayerJoinRequest @0x5122a9..0x5122f0]; the squad tag bare [orig:
	// NapiNPServer_HandleChatMessage @0x513930]; the rules word as built [orig:
	// CNapiGameSession_InitRandomSeedOrRequest @0x51e94b..0x51e957].
	p.in_match = ProtocolInMatch{};
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
	// NovaLogic's live gate answered JO's tag with no POST pair, an http://[domainname] startupurl
	// and a TimeOnGateServer line after the reflected pair (the LN S1 retail host record,
	// 2026-10-23), as it answered DFX2's: the reply the service's gate gives this protocol.
	// [orig: CNapiGateManager_ProcessResponse @0x4CED20 - the in-use word set by a udpnovaworld
	//  line @0x4CF541 skips the POST tests @0x4CF5A0, and no key reads TimeOnGateServer;
	//  Mission_DeobfuscateDescription @0x4CDAA0 - [DOMAINNAME] @0x4CDB88]
	p.gate_reply_witnessed = true;
	// jop_2_login.mnx's submit is ACCEPT: no LOGIN control.
	p.sign_in_submit = "ACCEPT";
	// [orig: CNapiNetwork_Init @0x4ca76c, the literal @0x7ca6d8]
	p.bdat = "Jul 21 2009 18:54:42";
	// The lobby's second build-date literal, one second before the network init's.
	// [orig: CNapiGameSession_InitNPConnection @0x4d3ce1 and CNapiGameSession_ConnectToNovaWorld
	//  @0x4d479d, the literal @0x7cc760]
	p.lobby_bdat = "Jul 21 2009 18:54:41";
	// PV3's version 1, 7, 5, 7 (ebp = 1 @0x4ca5fc) and its date.
	// [orig: CNapiNetwork_Init @0x4ca83d..0x4ca85b; the date @0x4ca894]
	ProtocolBuild build;
	build.version = { 1u, 7u, 5u, 7u };
	build.date = "Jul 21 2009 18:54:42";
	p.build = build;
	// The version text, the binary's own sprintf(buffer, "V%i.%i.%i.%i", ...) at startup, the four
	// numbers pushed as immediates or as a register the function set (the build's packed version
	// dword, stored beside it, agrees): 1, 7, 5, 7 into the menu's buffer and the session's,
	// ebx = 1 [orig: Game_ParseCommandLineAndInit @ 0x4a7310 — ebx @ 0x4a737b, the menu's
	// @ 0x4a7d5c..0x4a7d6d (read by UI_OnStartupScreenActivate @ 0x555852), the session's
	// @ 0x4a7d75..0x4a7d90 (the ClientAuth copy @ 0x569b56), the dword 0x01070507 @ 0x4a7d86].
	p.version_text = "V1.7.5.7";
	return p;
}

} // namespace

const Protocol &jo_protocol() {
	static const Protocol protocol = make();
	return protocol;
}

} // namespace opennova::games
