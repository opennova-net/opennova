#include "dfx2.h"

#include <games/novalogic_2009/generation.h>

namespace opennova::games {

namespace {

// dfx2.exe's protocol over the 2009 generation. Its network init is the twin of
// [orig: CNapiNetwork_Init @0x4ca4a0] at [orig: @0x4ca610 (dfx2.exe)]. Its engine build is JO's,
// 1.7.5.7 (its dates its own), which ONE's build replaces in every slot (Protocol,
// net/protocol/protocol.h); its lobby protocol is JO's but for that date.
Protocol make() {
	Protocol p;
	p.code = "dfx2";
	p.name = "DFX2";
	// [orig: @0x4ca900 (dfx2.exe)]
	p.pn = "DELTAFORCEXTREME2";
	// QUuid(0xC3E094A6, 0xB749, 0x4B60, 0xBB,0x66,0x4A,0x04,0x38,0xEF,0x9C,0xEB), laid out as JO's.
	// [orig: the static initializer @0x7923c0..0x7923e6 (dfx2.exe); copied @0x4ca914 (dfx2.exe)]
	p.guid = std::array<uint8_t, 16>{ 0xA6, 0x94, 0xE0, 0xC3, 0x49, 0xB7, 0x60, 0x4B, 0xBB, 0x66,
		0x4A, 0x04, 0x38, 0xEF, 0x9C, 0xEB };
	// [orig: @0x4ca932 (dfx2.exe)]
	p.pv1 = "1.0.0 3-23-2005 EM dfxowkjzf";
	// PV3's bracket pair: the addresses of the "1" literal @0x7c8540 (8160576) and the "9013"
	// literal @0x7c8524 (8160548), the version pair's own strings.
	// [orig: the pushes @0x4ca9b2 / @0x4ca9ad, the sprintf @0x4ca9cb (dfx2.exe)]
	p.pv3_tags = { 0x7C8540u, 0x7C8524u };
	p.generation = &novalogic_2009();
	// docs/titles/dfx2-vs-jo-re.md §7, the §8 rows each field names.
	ProtocolInMatch in_match;
	// DFX2-1: g_CoopCheckpointSlot after the four values.
	// [orig: NetPacket_WritePlayerState @0x4fcd84..0x4fcd9b (dfx2.exe); NapiNPClientMsg_0x00A
	//  @0x431dee..0x431e01 (dfx2.exe)]
	in_match.objective_checkpoint = true;
	// DFX2-2: the flags byte starts at 1. [orig: NetPacket_WriteWorldStateLoad0x0F @0x4ffeed
	//  (dfx2.exe)]
	in_match.spawn_zone_flag_always = true;
	// DFX2-10: 3 is armory-only. [orig: NetPacket_SerializeWeaponRestrictionTable @0x50d3b0 /
	//  @0x50d3de (dfx2.exe); NapiNPClientMsg_HandleWeaponRestrictions @0x42f2ac (dfx2.exe);
	//  CAdminServer_HandleWeaponCommand @0x40469a, @0x40480e (dfx2.exe)]
	in_match.armory_only_availability = 3;
	// DFX2-11: [orig: Server_OnPlayerJoin @0x517717 (dfx2.exe); Server_DeployPlayer @0x514a55
	//  (dfx2.exe)]
	in_match.join_state_fixed = true;
	// DFX2-12: [orig: NetPacket_SendLoadoutSubmit @0x42ec1f..0x42ec3e (dfx2.exe)]
	in_match.loadout_class_fixed = true;
	// DFX2-13: the LAN CD-key leg after the app id, the chains cdkey.h's.
	// [orig: Server_ValidatePlayerJoinRequest @0x50f3e2..0x50f565 (dfx2.exe)]
	in_match.lan_cd_key_check = true;
	// DFX2-14: [orig: NapiNPServer_HandleChatMessage @0x510b32..0x510b6a (dfx2.exe)]
	in_match.chat_squad_tag_wrapped = true;
	// DFX2-18: [orig: CNapiGameSession_InitRandomSeedOrRequest @0x51b89e (dfx2.exe)]
	in_match.rules_word_bits = 0x10000u;
	p.in_match = in_match;
	// [orig: @0x4fc4af, @0x4fc4c5 (dfx2.exe), the twin of Lobby_UpdateServerInfo @0x4fe8c0;
	//  [VER1] @0x4cdc8c, [VER2] @0x4cdca3 in the twin of Mission_DeobfuscateDescription @0x4cdc30
	//  (dfx2.exe)]
	p.ver1 = "1";
	p.ver2 = "9013";
	// [orig: [PRODUCTCODE] @0x4cdce8 (dfx2.exe)]
	p.product_code = "dfx2";
	// [orig: @0x4d1699 (dfx2.exe), the twin of CNapiGateManager_InitDefaults @0x4d1460]
	p.gate_tag = "dfx2:0:cus:buffy";
	// [orig: pushed @0x4cdec7 in @0x4cde60 (dfx2.exe), the twin of Mission_LoadEncryptedConfig
	//  @0x4cdcd0]
	p.gate_tag_keys = "dfx2:0:buffy";
	// DFXCATEGORY1 after LAN on both host lists (DFX2-7).
	// [orig: CNapiGameSession_BuildHostVarLists @0x4d0ce0 (dfx2.exe) - the HostSetup list's
	//  DFXCATEGORY1 @0x4d0e8a..0x4d0eb1, the Host list's @0x4d0faf..0x4d0fd5]
	p.host_category_var = true;
	// NovaLogic's live gate answered the DFX2 tag with no POST pair, an http://[domainname]
	// startupurl and a TimeOnGateServer line (docs/titles/dfx2-vs-jo-re.md §6.5): the reply the
	// service's gate gives this protocol. [orig: CNapiGateManager_ProcessResponse @0x4CED20 - the
	//  in-use word's skip of the POST tests @0x4CF541 / @0x4CF5A0, no key for TimeOnGateServer
	//  (dfx2.exe's twin @0x4CEEB0 identical); Mission_DeobfuscateDescription @0x4CDAA0 -
	//  [DOMAINNAME] @0x4CDB88, called from CNapiGameSession_OnNovaWorldConnected @0x4D1665]
	p.gate_reply_witnessed = true;
	// dfx2_0_login.mnx's submit is LOGIN (its FORM_POST to NWLogin.dll).
	p.sign_in_submit = "LOGIN";
	return p;
}

} // namespace

const Protocol &dfx2_protocol() {
	static const Protocol protocol = make();
	return protocol;
}

} // namespace opennova::games
