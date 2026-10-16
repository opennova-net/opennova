// The server-info update's session side (server_info.h).
#include <runtime/inmatch/server_info.h>

#include <base/gameprofile/game_type.h> // host_abbreviation_key
#include <formats/mission/bms.h>
#include <net/npwire/peer_addr.h> // peer_addr_to_string (PlayerIpAndPort)
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/world/world.h>

#include <string>

namespace opennova::inmatch {

void read_session_listing_columns(
		HostRegistration &r, const NapiNPServerCtx &ctx, const hud::GameTextLookup &gametext) {
	const GameConfig &config = ctx.config;
	// [orig: g_ServerNameStr @0x24D1FC4, pushed @0x4FE92C]
	r.server_name = config.server_name;
	// GameType_GetAbbreviation(g_GameType, 1): the game type's GateTypeAbbrev text
	// [orig: @0x4FE942..0x4FE94A, the SetOrCreate @0x4FE984]
	r.game_type = hud::game_text(
			gametext, "GateTypeAbbrev", game_type::host_abbreviation_key(config.game_type), "");
	// The mission text's [info] title, else, when it reads empty, the mission header's name
	// [orig: MissionText_GetString("info", "title") @0x4FE989..0x4FE993; the strlen test and
	//  dword_A761D4, the header's mission_name (header +4), @0x4FE99D..0x4FE9AD; the SetOrCreate
	//  @0x4FE9D0]
	if (!ctx.mission_text_title.empty())
		r.mission_name = ctx.mission_text_title;
	else if (ctx.mission != nullptr)
		r.mission_name = ctx.mission->get_mission_name();
	// Region: the header's climate (lod_level, header +0x84), 0..2 the STRNOVA07..09 text
	// [orig: `mov eax, dword_A76254` @0x4FE9D5, the switch @0x4FE9DA..0x4FE9E7]
	if (ctx.mission != nullptr)
		r.region_index = static_cast<int>(ctx.mission->header.climate);
	// [orig: dword_24D211C @0x4FEB03]
	r.published_cap = static_cast<int>(config.player_slot_limit());
	// [orig: g_ServerJoinLocked @0xC94794 — Locked @0x4FEC57, Country @0x4FEEED]
	r.locked = ctx.join_locked;
	// [orig: dword_24D218C @0x4FECD7]
	r.skins = config.allow_custom_skins != 0;
	// [orig: g_RoundTimeRemaining @0x24C1958, read @0x4FED57]
	r.round_time_remaining_ticks = ctx.world != nullptr ? ctx.world->match.remaining_ticks() : -1;
	// [orig: the strlen of the password @0x24D1FE4 @0x4FEDCA..0x4FEDDD]
	r.password = !config.server_password.empty();
	// [orig: `test byte g_RulesFlags, 1` @0x4FEE48]
	r.tracers = (config.mp_attributes & GameConfig::kMpAttribNoTracers) == 0;
	// Country: the session's country code ("XX" while locked, " " for "XX" or none: the builder)
	// [orig: g_SessionCountryCode @0x24D2017 @0x4FEEFB..0x4FEF30]
	r.country = config.country;
	// [orig: g_ServerCustomText @0x24D21C4, pushed @0x4FEF46]
	r.server_message = config.custom_text;
}

bool Server_ToggleJoinLock(NapiNPServerCtx &ctx) {
	// [orig: `cmp g_ServerJoinLocked, eax(0); sete al; mov g_ServerJoinLocked, eax`
	//  @0x50D750..0x50D75D]
	ctx.join_locked = !ctx.join_locked;
	// [orig: `push 0; call Lobby_UpdateServerInfo` @0x50D758 / @0x50D762]
	if (ctx.on_update_server_info)
		ctx.on_update_server_info();
	return ctx.join_locked;
}

std::vector<HostPlayerListRow> host_player_list_rows(const NapiNPServerCtx &ctx) {
	std::vector<HostPlayerListRow> rows;
	for (const NapiNPConnection &c : ctx.np_protocol.connection_list) {
		if (c.phase < ConnectionPhase::PlayerAdded) continue;
		HostPlayerListRow row;
		row.own = c.type == NapiNPConnection::kTypeClientSide;
		row.slot.slot = static_cast<int>(c.reply.player_slot);
		row.slot.connection_id = c.connection_id;
		// The roster's name is the joiner's NA bytes, the page's (D-NET-661).
		row.slot.player_name = c.player_name;
		row.slot.ip_and_port = peer_addr_to_string(c.peer);
		row.slot.pcid = c.account.pcid;
		row.slot.team = std::to_string(c.player_list_team);
		row.slot.type = std::to_string(c.player_list_type);
		rows.push_back(std::move(row));
	}
	return rows;
}

} // namespace opennova::inmatch
