#include <runtime/inmatch/server_join_check.h>

#include <runtime/inmatch/game_config.h>
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_ban_lists.h> // Server_ValidateJoinBans (29 / 30 / 31)

#include <base/io/crt_ftol.h>  // retail_atol (the VERSIONCRCSTRING compare)
#include <base/io/strutil.h>   // iequals (Napi_StrCaseEqual)
#include <net/novacrypto/cdkey.h> // check_lan_join (the CD-key leg)
#include <net/npwire/flat_tlv.h>      // the C2S 0x00 JOIN body walk
#include <net/title/title_identity.h> // the title's build values and CD-key chains

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::inmatch {

namespace {

// The JOIN body's TLV walk: unknown fields pass, the last matching value wins, and only an
// empty name stops it (read_flat_tlv; @0x512B64); tag names compare through Napi_StrCaseEqual
// @0x616e70. Each string copies from its value to the first NUL whatever its length
// (load_value_string), into the handler's buffer when it has one.
// [orig: NapiNPServer_HandlePlayerJoinMessage @0x512aa0]
template <typename Fn>
void walk_join_tlvs(const std::vector<uint8_t> &payload, Fn &&fn) {
	std::size_t pos = 0;
	const uint8_t *end = payload.data() + payload.size();
	while (pos < payload.size()) {
		FlatTlvField field;
		const size_t next = read_flat_tlv(payload.data(), payload.size(), pos, field);
		if (next == kFlatTlvEnd || field.name.empty()) break;
		pos = next;
		fn(field, end);
	}
}

// Literal patch/account gates precede the expansion and spectator legs, each against the
// title's build values (TitleJoinBuild). Missing CU fields retain zero and fail here, after the
// NP handshake.
// [orig: Server_ValidatePlayerJoinRequest @0x512100 - @0x5121b5, @0x5121f3, @0x51221a,
//  @0x51223f / @0x51225f, @0x512286] [orig: @0x50f2cb..0x50f382 (dfx2.exe)]
uint32_t validate_join_environment(const ClientGameEnvironment &env, const TitleJoinBuild &build) {
	if (env.bn != build.bn) return 2;
	if (env.vn != build.vn) return 3;
	if (env.mbn != build.mbn) return 4;
	if (env.bt == 1) return 6;
	if (env.bt == 2) return 7;
	if (env.sopd != build.sopd) return 8;
	return 0;
}

// The NovaWorld app-id leg (code 9), after the build values and ahead of the expansion's (and
// DFX2's CD-key leg): in a NovaWorld session a joiner whose ClientAuth APPID differs from the
// session's own AppId (ctx+0x1194) is refused, an absent APPID reading 0. A LAN session runs none
// of it.
// [orig: Server_ValidatePlayerJoinRequest @0x512100 - is_in_session @0x5122A3, transport_mode
//  NovaWorld @0x5122AB, the joiner's +0x10C @0x5122B3, CNapiNPCtx_GetAppId @0x5122BE, the compare
//  @0x5122C3, the refusal 9 @0x5122C7..0x5122DD]
uint32_t validate_novaworld_app_id(const NapiNPServerCtx &ctx, const NapiNPConnection &conn) {
	if (ctx.is_in_session == 0 || ctx.transport_mode != NetworkType::NovaWorld) return 0;
	return static_cast<uint32_t>(conn.join_environment.app_id) == ctx.novaworld_app_id ? 0 : 9;
}

// The CD-key leg of DFX2's rewritten check, after the app id (code 9), in a LAN session only
// (in session and network type 2; a NovaWorld session runs none of it): cdkey::check_lan_join
// over the joiner's ids, the host's own and every other player's. JO's check has no such leg
// (TitleInMatch::lan_cd_key_check).
// [orig: Server_ValidatePlayerJoinRequest @0x50f1d0 (dfx2.exe) - the LAN test
//  @0x50f3e2..0x50f3f6, the player walk @0x50f486..0x50f4c6 (not the joiner @0x50f494)]
uint32_t validate_lan_cd_key(const NapiNPServerCtx &ctx, const TitleInMatch &rules,
		const NapiNPConnection &conn, const std::vector<NapiNPConnection> &roster) {
	if (!rules.lan_cd_key_check) return 0;
	if (ctx.is_in_session == 0 || ctx.transport_mode != NetworkType::Lan) return 0;
	std::vector<cdkey::WireKeys> others;
	for (const NapiNPConnection &other : roster)
		if (&other != &conn) others.push_back(other.cd_keys);
	return static_cast<uint32_t>(cdkey::check_lan_join(conn.cd_keys, ctx.host_cd_keys, others));
}

uint32_t validate_join_request(
		const GameConfig &config, const std::vector<uint8_t> &payload) {
	std::string expansion;
	std::string version_crc;
	// [orig: NapiNPServer_HandlePlayerJoinMessage @0x512aa0 - EXP into 32 bytes @0x512BFF,
	//  VERSIONCRCSTRING into 512 @0x512C31]
	walk_join_tlvs(payload, [&](const FlatTlvField &field, const uint8_t *end) {
		if (strutil::iequals(field.name, "EXP"))
			expansion = load_value_string(field.value, end, 32);
		else if (strutil::iequals(field.name, "VERSIONCRCSTRING"))
			version_crc = load_value_string(field.value, end, 512);
	});
	// [orig: Server_ValidatePlayerJoinRequest @0x5122fc..0x512349]
	if (expansion != config.expansion) return 47;
	if (!config.expansion.empty() &&
	    io::retail_atol(version_crc.c_str()) != // the CRT atol @0x512327 (D-NET-384)
	            config.expansion_version_checksum) return 48;
	return 0;
}

// The game-layer spectator gate, run at the 0x00 join message AFTER the
// NP-level 0x42 admitted the connection. Order and codes are the witnessed
// legs: 14 spectating disabled, 15 positive slot count exceeded, 16 password
// mismatch (case-insensitive, like the side passwords). The candidate's flag
// is latched BEFORE the count, so the count includes it and rejects on
// strictly-greater — retail's exact shape. A -1 (shared-capacity) setting has
// no spectator-count leg; the shared 0x42 gate already bounded the total.
// [orig: the entry+55 latch from the stored JSR in
// NapiNPServer_HandlePlayerJoinMessage @0x512aa0; the disabled leg @0x512364
// (code 14), the `> g_GameConfigState.maxSpectators_26C` count @0x5123ca (code 15, type-1 rows
// with entry+55), the Napi_StrCaseEqual JSPP compare @0x512405..0x512425
// (code 16); Server_ValidatePlayerJoinRequest @0x512100]
uint32_t validate_spectator_join(const GameConfig &config,
		NapiNPConnection &conn, const std::vector<NapiNPConnection> &roster) {
	conn.link.spectator = conn.join_spectator_request != 0;
	if (!conn.link.spectator) return 0;
	if (config.spectator_slots == 0) return 14;
	if (config.spectator_slots > 0) {
		uint32_t spectators = 0;
		for (const NapiNPConnection &row : roster) {
			if (row.type == NapiNPConnection::kTypeServerSide &&
			    row.phase >= ConnectionPhase::Joined && row.link.spectator)
				++spectators;
		}
		if (spectators > static_cast<uint32_t>(config.spectator_slots))
			return 15;
	}
	// [orig: Napi_StrCaseEqual @0x616e70]
	if (!config.spectator_password.empty() &&
	    !strutil::iequals(
				conn.join_spectator_password, config.spectator_password))
		return 16;
	return 0;
}

// The side-password leg follows spectator admission. TR is already narrowed
// by the auth tag loader; -1 accepts either password (or an unlocked side).
// [orig: Server_ValidatePlayerJoinRequest @0x5124A2..0x5125D4; the password
//  compare Napi_StrCaseEqual @0x616e70]
uint32_t validate_side_password(const GameConfig &config, const NapiNPConnection &conn) {
	if ((config.mp_attributes & GameConfig::kMpAttribTeamChoose) == 0 ||
			conn.link.spectator) return 0;
	const auto matches = [&](const std::string &password) {
		return password.empty() || strutil::iequals(password, conn.join_password);
	};
	switch (conn.char_vars.team_request) {
		case 0xFF: return matches(config.side_a_password) || matches(config.side_b_password) ? 0 : 18;
		case 0: return matches(config.side_a_password) ? 0 : 19;
		case 1: return matches(config.side_b_password) ? 0 : 20;
		default: return 22;
	}
}


} // namespace

void read_join_cd_key_ids(NapiNPConnection &conn, const std::vector<uint8_t> &join_payload) {
	walk_join_tlvs(join_payload, [&](const FlatTlvField &field, const uint8_t *end) {
		// Napi_SetString keeps the whole C string (a heap copy, no buffer).
		if (strutil::iequals(field.name, "CDKIID"))
			conn.cd_keys.cdkiid = load_value_string(field.value, end, SIZE_MAX);
		else if (strutil::iequals(field.name, "CDKIIDEXP1"))
			conn.cd_keys.exp1 = load_value_string(field.value, end, SIZE_MAX);
	});
}

uint32_t Server_ValidatePlayerJoinRequest(const GameConfig &config, NapiNPServerCtx *ctx,
		NapiNPConnection &conn, const std::vector<NapiNPConnection> &roster,
		const std::vector<uint8_t> &join_payload, bool pcid_decrypted) {
	const TitleIdentity *title = ctx != nullptr ? ctx->np_protocol.identity : nullptr;
	uint32_t reject_dpc = validate_join_environment(conn.join_environment, title_join_build(title));
	if (reject_dpc == 0 && ctx != nullptr)
		reject_dpc = validate_novaworld_app_id(*ctx, conn);
	if (reject_dpc == 0 && ctx != nullptr)
		reject_dpc = validate_lan_cd_key(*ctx, title_in_match(title), conn, roster);
	if (reject_dpc == 0)
		reject_dpc = validate_join_request(config, join_payload);
	if (reject_dpc == 0)
		reject_dpc = validate_spectator_join(config, conn, roster);
	if (reject_dpc == 0)
		reject_dpc = validate_side_password(config, conn);
	if (reject_dpc == 0 && ctx != nullptr)
		reject_dpc = Server_ValidateJoinBans(*ctx, conn, pcid_decrypted); // 29 / 30 / 31
	return reject_dpc;
}

} // namespace opennova::inmatch
