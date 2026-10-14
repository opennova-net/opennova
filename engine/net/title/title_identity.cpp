#include <net/title/title_identity.h>

#include <base/gameprofile/gameprofile.h>
#include <base/io/strutil.h>
#include <base/resource_index/resource_index.h>
#include <net/novacrypto/pubcrypto.h>

#include <algorithm>
#include <array>

using namespace opennova::gameprofile;

namespace opennova {

namespace {

// The NAPI library's protocol version string, the same text in every JO-family executable: the
// announce and the join write it and the host's 0x41 / 0x42 gates compare it
// (docs/titles/dfx2-vs-jo-re.md §6.1).
// [orig: the literal @0x7dfcf0, written NapiNPSession_SendAnnouncePacket @0x61fa92 and
//  CNapiNPConnection_SendClientJoin @0x61fea7, compared NapiNPProtocol_HandleClientHello
//  @0x6217cf and NapiNPProtocol_HandleClientJoin @0x62bcdc] [orig: the literal @0x7f9280,
//  written @0x707bc2 / @0x707fd7, compared @0x7098ff / @0x713e0c (dfx2.exe)]
constexpr const char *kNapiNpVersion =
		"NAPI NP Version 0.0.1 1/12/2004 - 2/20/2004 Milota Copyright 2004 NovaLogic";

// The company both network inits write, and the lobby session's.
// [orig: CNapiNetwork_Init @0x4ca72f; CNapiGameSession_InitNPConnection @0x4d3cae]
// [orig: @0x4ca89f, @0x4d3e9e (dfx2.exe)]
constexpr const char *kNovaLogicCompany = "NovaLogic Inc, Calabasas CA U.S.A.";

// Joint Operations: Typhoon Rising, the JO:CA Jointops.exe (docs/engine-primer.md's IDB).
TitleIdentity jo_row() {
	TitleIdentity t;
	t.game_id = GAME_JO;
	t.nvs = kNapiNpVersion;
	t.co = kNovaLogicCompany;
	// The executable's own file name, which the network init reads off its command line: the
	// stock install's Jointops.exe. [orig: CNapiNetwork_Init @0x4ca747 -> Napi_GetExeBasename
	//  @0x62e0b0, copied to proto+0x58 @0x4ca759]
	t.ap = "Jointops.exe";
	// [orig: CNapiNetwork_Init @0x4ca76c, the literal @0x7ca6d8]
	t.bdat = "Jul 21 2009 18:54:42";
	// [orig: CNapiNetwork_Init @0x4ca790, the literal @0x7ca6c8]
	t.pn = "JOINTOPERATIONS";
	// QUuid(0xB074D646, 0x81F9, 0x475F, 0x92,0xDA,0xDE,0xA7,0x24,0x7F,0x14,0x68): QUuid's first
	// three components occupy native little-endian memory, and PG copies those 16 bytes.
	// [orig: the static initializer @0x7937a0 into g_GameProtocolGuid; CNapiNetwork_Init
	//  @0x4ca7a4..0x4ca7af -> NapiNPVarBlock_Copy(proto+0x11C)]
	t.guid = std::array<uint8_t, 16>{0x46, 0xD6, 0x74, 0xB0, 0xF9, 0x81, 0x5F, 0x47,
	                                 0x92, 0xDA, 0xDE, 0xA7, 0x24, 0x7F, 0x14, 0x68};
	// [orig: CNapiNetwork_Init @0x4ca7c2, the literal @0x7ca6b4]
	t.pv1 = "0.0.0 1/12/2004 EM";
	// [orig: CNapiNetwork_Init @0x4ca7db, the literal @0x7ca6b0]
	t.pv2 = "16";
	// The version 1, 7, 5, 7 (ebp = 1 @0x4ca5fc) and the addresses of the "3" literal @0x7c9730
	// (8165168) and the "2345" literal @0x7c9714 (8165140), the version pair's own strings.
	// [orig: CNapiNetwork_Init @0x4ca83d..0x4ca85b; the date @0x4ca894]
	TitleBuild build;
	build.version = {1u, 7u, 5u, 7u};
	build.tag_address_a = 0x7C9730u;
	build.tag_address_b = 0x7C9714u;
	build.date = "Jul 21 2009 18:54:42";
	t.build = build;
	// The joiner's CU values, its net config's defaults, and the join check's build gates on the
	// same constants. [orig: NapiNetConfig_Init @0x4c3570 - VN @0x4c3622, BN @0x4c3629, MBN
	//  @0x4c3630, SOPD @0x4c3637; CNapiServerInfo_SerializeToSession @0x4c3650 - VN @0x4c36ed,
	//  BN @0x4c372c, MBN @0x4c37ac, SOPD @0x4c37ed; Server_ValidatePlayerJoinRequest @0x512100 -
	//  BN 1 @0x5121b5 (code 2 @0x5121df), VN 2 @0x5121f3 (3), MBN 0x131D112 @0x51221a (4), SOPD
	//  180 @0x512286 (8)]
	t.join_build = TitleJoinBuild{1, 2, 20042002, 180};
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
	t.in_match = TitleInMatch{};
	// [orig: CNapiGameSession_InitNPConnection @0x4d3d51, the literal @0x7cc73c]
	t.lobby_pv1 = "0.0.0 2/10/2004 EM";
	// The lobby's second build-date literal, one second before the network init's.
	// [orig: CNapiGameSession_InitNPConnection @0x4d3ce1 and CNapiGameSession_ConnectToNovaWorld
	//  @0x4d479d, the literal @0x7cc760]
	t.lobby_bdat = "Jul 21 2009 18:54:41";
	// [orig: Lobby_UpdateServerInfo @0x4ff3ea / @0x4ff3ff; Mission_DeobfuscateDescription
	//  @0x4cdb9f / @0x4cdbb6 - the literals @0x7c9730 / @0x7c9714]
	t.ver1 = "3";
	t.ver2 = "2345";
	// [orig: Mission_DeobfuscateDescription @0x4cdbfe]
	t.product_code = "jop";
	// [orig: CNapiGateManager_InitDefaults @0x4d14b6, the literal @0x7cc35c]
	t.gate_tag = "jop:cus2";
	// [orig: Mission_LoadEncryptedConfig @0x4cdd65, the literal @0x7cbb44]
	t.gate_tag_keys = "jop:2:oyez";
	// The host lists end at LAN. [orig: CNapiGameSession_BuildHostVarLists @0x4d0b50]
	t.host_category_var = false;
	// The retail jop_2 pages' pfid (docs/net/novaworld-net-re.md §2, the NWLogin row).
	t.pfid = 28;
	// The retail jop_2 pages (docs/net/novaworld-net-re.md §2 and its wave 6, the authenticated web
	// flow against the real service).
	TitlePages pages;
	pages.msgbase = "jop_2_msg.htm";
	pages.main = "jop_2_main.htm";
	pages.login = "jop_2_login.htm";
	pages.version_file = "jop_2.ver";
	pages.new_update = "jop_2_newupdateavailable.htm";
	pages.new_update_bypass = "jop_2_newupdateavailable2.htm";
	pages.junction = "jop_2_junction.htm";
	pages.need_to_agree = "jop_2_needtoagree.htm";
	pages.no_db = "jop_2_nodb.htm";
	pages.relay = "jop_2_relay.htm";
	pages.enter_key = "jop_2_key.htm";
	pages.key_error = "jop_2_key2err.htm";
	pages.join = "jop_2_join.joi";
	pages.server_list = "jop_2.gsb";
	t.pages = pages;
	return t;
}

// The Joint Operations demo (jodemo.exe): its gate tag alone is witnessed
// (docs/net/novaworld-net-re.md §6.7).
TitleIdentity jodemo_row() {
	TitleIdentity t;
	t.game_id = GAME_JO_DEMO;
	t.gate_tag = "jopd:cus4";
	return t;
}

// Delta Force: Xtreme: no binary of its read for its identity.
TitleIdentity dfx_row() {
	TitleIdentity t;
	t.game_id = GAME_DFX;
	return t;
}

// Delta Force: Xtreme 2 (dfx2.exe, the DFX2 install's): JO's code over its own data
// (docs/titles/dfx2-vs-jo-re.md §6, DFX2-3). Its network init is the twin of
// [orig: CNapiNetwork_Init @0x4ca4a0] at [orig: @0x4ca610 (dfx2.exe)].
TitleIdentity dfx2_row() {
	TitleIdentity t;
	t.game_id = GAME_DFX2;
	t.nvs = kNapiNpVersion;
	t.co = kNovaLogicCompany;
	// The executable's own file name: the stock install's dfx2.exe.
	// [orig: @0x4ca8b7 -> the twin of Napi_GetExeBasename @0x7161e0 (dfx2.exe)]
	t.ap = "dfx2.exe";
	// The __DATE__ form, two spaces before the 7. [orig: @0x4ca8dc (dfx2.exe)]
	t.bdat = "Jul  7 2009 11:33:09";
	// [orig: @0x4ca900 (dfx2.exe)]
	t.pn = "DELTAFORCEXTREME2";
	// QUuid(0xC3E094A6, 0xB749, 0x4B60, 0xBB,0x66,0x4A,0x04,0x38,0xEF,0x9C,0xEB), laid out as JO's.
	// [orig: the static initializer @0x7923c0..0x7923e6 (dfx2.exe); copied @0x4ca914 (dfx2.exe)]
	t.guid = std::array<uint8_t, 16>{0xA6, 0x94, 0xE0, 0xC3, 0x49, 0xB7, 0x60, 0x4B,
	                                 0xBB, 0x66, 0x4A, 0x04, 0x38, 0xEF, 0x9C, 0xEB};
	// [orig: @0x4ca932 (dfx2.exe)]
	t.pv1 = "1.0.0 3-23-2005 EM dfxowkjzf";
	// [orig: @0x4ca94b (dfx2.exe)]
	t.pv2 = "16";
	// The version 1, 7, 5, 7 and the addresses of the "1" literal @0x7c8540 (8160576) and the
	// "9013" literal @0x7c8524 (8160548), the version pair's own strings.
	// [orig: the pushes @0x4ca9b2 / @0x4ca9ad, the sprintf @0x4ca9cb, the date
	//  @0x4ca9f0..0x4caa0a, the copy to proto+0x1AC @0x4caa2b..0x4caa32 (dfx2.exe)]
	TitleBuild build;
	build.version = {1u, 7u, 5u, 7u};
	build.tag_address_a = 0x7C8540u;
	build.tag_address_b = 0x7C8524u;
	build.date = "Jul  7 2009 11:33:09";
	t.build = build;
	// JO's values: the same net config defaults and serializer, and the rewritten join check
	// keeps the build gates. [orig: NapiNetConfig_Init @0x4c3650 (dfx2.exe) - VN @0x4c3702, BN
	//  @0x4c3709, MBN @0x4c3710, SOPD @0x4c3717; CNapiServerInfo_SerializeToSession @0x4c3730
	//  (dfx2.exe); Server_ValidatePlayerJoinRequest
	//  @0x50f1d0 (dfx2.exe) - BN 1 @0x50f2cb (code 2), VN 2 @0x50f2ef (3), MBN 0x131D112
	//  @0x50f316 (4), SOPD 180 @0x50f382 (8)]
	t.join_build = TitleJoinBuild{1, 2, 20042002, 180};
	// docs/titles/dfx2-vs-jo-re.md §7, the §8 rows each field names.
	TitleInMatch in_match;
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
	t.in_match = in_match;
	// The lobby protocol is JO's but for its build date.
	// [orig: @0x4d3f41 in the twin of CNapiGameSession_InitNPConnection @0x4d3dd0 (dfx2.exe)]
	t.lobby_pv1 = "0.0.0 2/10/2004 EM";
	// [orig: @0x4d3ed1, and the twin of CNapiGameSession_ConnectToNovaWorld @0x4d4830 @0x4d498d
	//  (dfx2.exe)]
	t.lobby_bdat = "Jul  7 2009 11:33:09";
	// [orig: @0x4fc4af, @0x4fc4c5 (dfx2.exe), the twin of Lobby_UpdateServerInfo @0x4fe8c0;
	//  [VER1] @0x4cdc8c, [VER2] @0x4cdca3 in the twin of Mission_DeobfuscateDescription @0x4cdc30
	//  (dfx2.exe)]
	t.ver1 = "1";
	t.ver2 = "9013";
	// [orig: [PRODUCTCODE] @0x4cdce8 (dfx2.exe)]
	t.product_code = "dfx2";
	// [orig: @0x4d1699 (dfx2.exe), the twin of CNapiGateManager_InitDefaults @0x4d1460]
	t.gate_tag = "dfx2:0:cus:buffy";
	// [orig: pushed @0x4cdec7 in @0x4cde60 (dfx2.exe), the twin of Mission_LoadEncryptedConfig
	//  @0x4cdcd0]
	t.gate_tag_keys = "dfx2:0:buffy";
	// DFXCATEGORY1 after LAN on both host lists (DFX2-7).
	// [orig: HostSetup @0x4d0e8a..0x4d0eb1, Host @0x4d0faf..0x4d0fd5 in the twin of
	//  CNapiGameSession_BuildHostVarLists @0x4d0ce0 (dfx2.exe)]
	t.host_category_var = true;
	// The live DFX2 gate's startupurl carries &pfid=38 (docs/titles/dfx2-vs-jo-re.md §6.5).
	t.pfid = 38;
	// The dfx2_0 pages the live gate's startupurl leads to (url=dfx2_0_start.htm, dfx2-vs-jo-re
	// §6.5), as the service serves them (apps/novaworld_server/templates): the start page's NWStart
	// link with its ACT page (dfx2_0_start.htm), the login form's fields (dfx2_0_login.htm: its
	// enterkey and its charglsvssrequest), the join link with its leading lan=0
	// (dfx2_0_main.mnx) and the dfx2_0.gsb feed.
	TitlePages pages;
	pages.msgbase = "dfx2_0_msg.htm";
	pages.main = "dfx2_0_main.htm";
	pages.login = "dfx2_0_login.htm";
	pages.version_file = "dfx2_0.ver";
	pages.new_update = "dfx2_0_newupdateavailable.htm";
	pages.new_update_bypass = "dfx2_0_newupdateavailable2.htm";
	pages.junction = "dfx2_0_junction.htm";
	pages.activate = "dfx2_0_actlogin.htm";
	pages.need_to_agree = "dfx2_0_needtoagree.htm";
	pages.no_db = "dfx2_0_nodb.htm";
	pages.relay = "dfx2_0_relay.htm";
	pages.enter_key = "dfx2_0_term.htm";
	pages.glsvss_request = "[dfx]kills";
	pages.join_lan = "0";
	pages.key_error = "dfx2_0_key2err.htm";
	pages.join = "dfx2_0_join.joi";
	pages.server_list = "dfx2_0.gsb";
	t.pages = pages;
	return t;
}

// Delta Force: Black Hawk Down: no binary of its read for its identity.
TitleIdentity bhd_row() {
	TitleIdentity t;
	t.game_id = GAME_BHD;
	return t;
}

const std::array<TitleIdentity, GAME_COUNT> &rows() {
	static const std::array<TitleIdentity, GAME_COUNT> table = {
		jo_row(), jodemo_row(), dfx_row(), dfx2_row(), bhd_row(),
	};
	return table;
}

void missing_if(std::vector<std::string> &out, bool unwitnessed, const char *name) {
	if (unwitnessed) out.emplace_back(name);
}

const char *part_name(TitleIdentityPart part) {
	return part == TitleIdentityPart::WebFlow ? "web flow" : "session";
}

} // namespace

const TitleIdentity *title_identity(int game_id) {
	for (const TitleIdentity &row : rows())
		if (row.game_id == game_id) return &row;
	return nullptr;
}

const TitleIdentity *title_identity_by_protocol_name(std::string_view pn) {
	for (const TitleIdentity &row : rows())
		if (row.pn != nullptr && strutil::iequals(pn, row.pn)) return &row;
	return nullptr;
}

std::vector<std::string> title_identity_missing(const TitleIdentity &t, TitleIdentityPart part) {
	std::vector<std::string> out;
	if (part == TitleIdentityPart::WebFlow) {
		missing_if(out, !t.pfid.has_value(), "pfid");
		missing_if(out, !t.pages.has_value(), "web pages");
		return out;
	}
	// The executable's version text, which the joiner's 0x42 carries as VERSIONSTRING, is the
	// game profile's (gameprofile.h GameProfile::version_text).
	const GameProfile *profile = gameprofile_by_id(t.game_id);
	missing_if(out, profile == nullptr || profile->version_text == nullptr, "version text");
	missing_if(out, t.nvs == nullptr, "NVS");
	missing_if(out, t.co == nullptr, "CO");
	missing_if(out, t.ap == nullptr, "AP");
	missing_if(out, t.bdat == nullptr, "BDAT");
	missing_if(out, t.pn == nullptr, "PN");
	missing_if(out, !t.guid.has_value(), "PG");
	missing_if(out, t.pv1 == nullptr, "PV1");
	missing_if(out, t.pv2 == nullptr, "PV2");
	missing_if(out, !t.build.has_value(), "PV3");
	missing_if(out, !t.join_build.has_value(), "join build values");
	missing_if(out, !t.in_match.has_value(), "in-match branches");
	missing_if(out, t.lobby_pv1 == nullptr, "lobby PV1");
	missing_if(out, t.lobby_bdat == nullptr, "lobby BDAT");
	missing_if(out, t.ver1 == nullptr, "Ver1");
	missing_if(out, t.ver2 == nullptr, "Ver2");
	missing_if(out, t.product_code == nullptr, "product code");
	missing_if(out, t.gate_tag == nullptr, "gate tag");
	missing_if(out, t.gate_tag_keys == nullptr, "gt.ssc key chain");
	return out;
}

bool title_identity_complete(const TitleIdentity &title, TitleIdentityPart part) {
	return title_identity_missing(title, part).empty();
}

std::string title_identity_refusal(const TitleIdentity &title, TitleIdentityPart part) {
	const std::vector<std::string> missing = title_identity_missing(title, part);
	if (missing.empty()) return std::string();
	const GameProfile *profile = gameprofile_by_id(title.game_id);
	std::string out = "the " + std::string(profile != nullptr ? profile->code : "?") + " title's " +
			part_name(part) + " identity is not witnessed yet: ";
	for (std::size_t i = 0; i < missing.size(); ++i) out += (i == 0 ? "" : ", ") + missing[i];
	return out;
}

const TitleInMatch &title_in_match(const TitleIdentity *title) {
	if (title != nullptr && title->in_match.has_value()) return *title->in_match;
	return *title_identity(GAME_JO)->in_match;
}

const TitleJoinBuild &title_join_build(const TitleIdentity *title) {
	if (title != nullptr && title->join_build.has_value()) return *title->join_build;
	return *title_identity(GAME_JO)->join_build;
}

const TitleIdentity *resolve_title_identity(std::string_view game_code, std::string &error) {
	const std::string code(game_code);
	const GameProfile *profile = gameprofile_by_code(code.c_str());
	const TitleIdentity *title = profile != nullptr ? title_identity(profile->id) : nullptr;
	if (title == nullptr) {
		error = "unknown game '" + code + "'";
		return nullptr;
	}
	const std::string refusal = title_identity_refusal(*title, TitleIdentityPart::Session);
	if (!refusal.empty()) {
		error = refusal;
		return nullptr;
	}
	return title;
}

bool decode_gate_tag_file(const std::vector<uint8_t> &bytes, const char *key_chain,
		std::string &tag) {
	if (key_chain == nullptr || bytes.size() > kGateTagFileMaxBytes) return false;
	// The read buffer is NUL-terminated at the file's size and handed over as a C string.
	const auto end = std::find(bytes.begin(), bytes.end(), uint8_t{0});
	const std::string text(bytes.begin(), end);
	std::vector<uint8_t> plaintext;
	if (!decode_key_chain(text, key_chain, plaintext, kGateTagDecodeRoom)) return false;
	// Napi_CopyString: the C string up to its NUL, at most 63 characters.
	const auto stop = std::find(plaintext.begin(), plaintext.end(), uint8_t{0});
	std::string copied(plaintext.begin(), stop);
	if (copied.size() > kGateTagChars) copied.resize(kGateTagChars);
	tag = std::move(copied);
	return true;
}

std::string title_gate_tag(const TitleIdentity &title, const ResourceIndex *mount) {
	std::string tag = title.gate_tag != nullptr ? title.gate_tag : "";
	if (mount == nullptr) return tag;
	std::vector<uint8_t> bytes;
	if (!mount->read_file_raw(kGateTagFile, bytes, VfsLookupPolicy::ForceLooseFirst)) return tag;
	(void)decode_gate_tag_file(bytes, title.gate_tag_keys, tag);
	return tag;
}

} // namespace opennova
