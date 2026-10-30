#include "dfx2.h"

#include <games/jo/jo.h>
#include <net/novacrypto/cdkey.h>

namespace opennova::games {

namespace {

// DFX2's CD key (DFX2-13). The CD-key descriptor, off_812EE8 (dfx2.exe), holds six chain
// pointers: +16 is the chain the LAN join check decodes a joiner's CDKIID under
// (CDKey_DecodeWireJoinChain @0x411b40), +20 the chain the registry value decodes under once
// mixed (CDKey_Decode @0x411af0); the other four point at the empty string. The CDKIIDEXP1
// descriptor, off_812F00 (dfx2.exe), the next six pointers, has every one at the empty string:
// a CDKIIDEXP1 decodes under the empty chain, one empty key.
const cdkey::KeyChains kCdKey = {
	// [orig: off_812EE8 +16 -> @0x7c2620 (dfx2.exe)]
	"5645236346346245:2345346541242546:4563451326353456",
	// [orig: off_812EE8 +20 -> @0x7c25e8 (dfx2.exe)]
	"928975689:234957945234:24598293879654:2349857982794876",
	// [orig: off_812F00 +16 -> @0x7bf886 (dfx2.exe)]
	"",
};

// dfx2.exe's protocol: Jointops.exe's row (jo_protocol.cpp), the same code over different data
// (docs/titles/dfx2-vs-jo-re.md §10), overridden where dfx2.exe's data differs: its identity, its
// in-match sheet by message and its NovaWorld values, each override cited. What it keeps of JO's
// row is dfx2.exe's too: the 2009 generation (NVS, CO, PV2 "16" and the join build, whose cites in
// games/novalogic_2009 name both executables) and the person index's 256. Its network init is the
// twin of [orig: CNapiNetwork_Init @0x4ca4a0] at [orig: @0x4ca610 (dfx2.exe)]. Its engine build is
// JO's, 1.7.5.7 (its dates its own), which ONE's build replaces in every slot (Protocol,
// net/protocol/protocol.h); its lobby protocol is JO's but for that date.
Protocol make() {
	Protocol p = jo_protocol();

	// --- Identity.
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

	// --- The in-match sheet: JO's, overridden by message (net/protocol/protocol.h
	// kProtocolDialect; docs/titles/dfx2-vs-jo-re.md §7, the §8 row each names).
	ProtocolInMatch &m = *p.in_match;
	// S2C 0x0A (DFX2-1): g_CoopCheckpointSlot after the four values.
	// [orig: NetPacket_WritePlayerState @0x4fcd84..0x4fcd9b (dfx2.exe); NapiNPClientMsg_0x00A
	//  @0x431dee..0x431e01 (dfx2.exe)]
	m.s2c_0a.objective_checkpoint = true;
	// S2C 0x0F (DFX2-2): the flags byte starts at 1.
	// [orig: NetPacket_WriteWorldStateLoad0x0F @0x4ffeed (dfx2.exe)]
	m.s2c_0f.spawn_zone_flag_always = true;
	// S2C 0x14 (DFX2-14): the squad tag between <ch> and <co>.
	// [orig: NapiNPServer_HandleChatMessage @0x510b32..0x510b6a (dfx2.exe)]
	m.s2c_14.chat_squad_tag_wrapped = true;
	// S2C 0x64 (DFX2-18): the rules word ORed with 0x10000.
	// [orig: CNapiGameSession_InitRandomSeedOrRequest @0x51b89e (dfx2.exe)]
	m.s2c_64.rules_word_bits = 0x10000u;
	// S2C 0x66 (DFX2-10): 3 is armory-only, the mission's 2.
	// [orig: NetPacket_SerializeWeaponRestrictionTable @0x50d3b0 / @0x50d3de (dfx2.exe);
	//  NapiNPClientMsg_HandleWeaponRestrictions @0x42f2ac (dfx2.exe);
	//  CAdminServer_HandleWeaponCommand @0x40469a, @0x40480e (dfx2.exe)]
	m.s2c_66.armory_only_availability = 3;
	// C2S 0x2F (DFX2-12): the class byte 8, or 7 in a mission of type 2.
	// [orig: NetPacket_SendLoadoutSubmit @0x42ec1f..0x42ec3e (dfx2.exe)]
	m.c2s_2f.loadout_class_fixed = true;
	// The slot state (DFX2-11): the join's 0x13, every deploy's 3.
	// [orig: Server_OnPlayerJoin @0x517717 (dfx2.exe); Server_DeployPlayer @0x514a55 (dfx2.exe)]
	m.slot_state.join_state_fixed = true;
	// C2S 0x00 (DFX2-13): the LAN CD-key leg after the app id, under these chains.
	// [orig: Server_ValidatePlayerJoinRequest @0x50f3e2..0x50f565 (dfx2.exe)]
	p.cd_key = &kCdKey;

	// --- NovaWorld.
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
