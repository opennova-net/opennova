#include "serve_listing.h"

#include "lister.h"

#include <base/io/log.h>
#include <runtime/inmatch/host_config.h> // run_host_server_command
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_info.h>
#include <runtime/inmatch/server_tick.h>

namespace opennova::serve {

using io::LogLevel;

void ServeListing::bind(inmatch::HostRole &role, mission::MissionKernel &kernel, Lister &lister) {
	role_ = &role;
	kernel_ = &kernel;
	lister_ = &lister;
	inmatch::NapiNPServerCtx &ctx = role.state.host_owner.ctx;
	// A service that asked for join tickets arms the join-phase watchdog: each validating joiner
	// is announced as a ClientPlayerEnterRequest on the hosting session and held until the
	// ServerPlayerEnterResult answers (on_player_enter_result) or the ticket deadline reaps it.
	// [orig: CNapiNetwork_CheckPlayerTimeouts @0x4C8AD0 — the arm @0x4C8B88,
	//  CNapiGameSession_SendPlayEnterRequest @0x4C8BC1]
	ctx.on_player_enter_request = [this](const inmatch::NapiNPServerCtx::PlayerEnterRequest &request) {
		if (lister_ == nullptr) return;
		lister_->host_role().request_player_enter(request.connection_id, request.peer.ip, request.peer.port,
		                                          request.join_ticket);
	};
	sync_session();
}

void ServeListing::unbind() {
	if (role_ != nullptr) {
		inmatch::NapiNPServerCtx &ctx = role_->state.host_owner.ctx;
		ctx.on_player_enter_request = nullptr;
		ctx.novaworld_join_tickets_armed = false;
		ctx.novaworld_gsid.clear();
		ctx.novaworld_app_id = 0;
	}
	role_ = nullptr;
	kernel_ = nullptr;
	lister_ = nullptr;
}

// The hosting session meets its match: the GSID the 0x81 advertises (cleared while the
// connection is down, re-supplied by a re-host), the AppId, the cookie-key ring the CD cookies
// decrypt under (it advances every refresh), the join-ticket arm, and the session's word, flags
// and exit store for the authority's NovaWorld exit. A dedicated server has no loopback, so no
// login PCID of its own.
// [orig: CNapiGameSession_HandleHostVerifyResponse @0x4D59D0 — the GSID @0x4D5BD6..0x4D5C0F;
//  Game_ProcessMainFrame's NovaWorld exit @0x52654f..0x52657c (dword_B5FD2C, dword_B60108)]
void ServeListing::sync_session() {
	if (role_ == nullptr || lister_ == nullptr) return;
	inmatch::NapiNPServerCtx &ctx = role_->state.host_owner.ctx;
	NwuHostRole &host = lister_->host_role();
	ctx.novaworld_gsid = host.gsid();
	ctx.novaworld_app_id = static_cast<uint32_t>(host.app_id());
	inmatch::set_novaworld_account_facts(ctx, host.cookie_keys(), std::string());
	ctx.novaworld_join_tickets_armed = host.requires_join_ticket();
	const NwuLobbySession::MatchFacts facts = lister_->lobby().match_facts();
	ctx.nwu_in_use = facts.in_use;
	ctx.nwu_session_role = facts.role;
	if (facts.exit_reason != 0) ctx.mission_exit_reason = facts.exit_reason;
}

bool ServeListing::start(int &exit_code) {
	(void)exit_code;
	return true;
}

// The live match changes every frame (the round clock, the joiners): once bound, every pass hands
// the host role the columns (they ride its 1860-tick refresh as its changed vars) and syncs the
// roster (ClientHostPlayerAdded / Removed go out at once, as Server_PlayerAdd sends them).
bool ServeListing::refresh(uint32_t now_ms, bool force) {
	(void)now_ms;
	return force || role_ != nullptr;
}

// The Host list's columns. HostSetup's MaxPlayers is the host file's cap, clamped by the lister
// to 1..65 for a dedicated host; the refreshed list's is the published cap less the dedicated
// slot, so neither counts the server's own slot. Dedicated is the STRNOVA11 text (listen_host
// false). Once the match binds, the session's columns are the live session's, which a
// ServerCommand or a map change edits (inmatch::read_session_listing_columns, the game's too).
// [orig: CNapiGameSession_BuildHostVarLists @0x4d0b50; CNapiGameSession_ConnectOrHost
//  @0x4d5046..0x4d507b; Lobby_UpdateServerInfo @0x4fe8c0 (@0x4feb03..0x4feb0a,
//  @0x4febe2..0x4fec52)]
HostRegistration ServeListing::registration() const {
	HostRegistration r = base_;
	r.listen_host = false;
	if (role_ == nullptr) return r;
	inmatch::read_session_listing_columns(r, role_->state.host_owner.ctx, seams_.gametext);
	return r;
}

// The match's PlayerList roster (inmatch::host_player_list_rows, the game shell's too). A mode-1
// host builds no local connection, so its own slot 0 is never published.
// [orig: CNapiGameSession_CreateSession @0x4c9b73]
std::vector<HostPlayerSlot> ServeListing::wanted_roster(const std::map<int, HostPlayerSlot> &current) const {
	(void)current;
	std::vector<HostPlayerSlot> out;
	if (role_ == nullptr) return out;
	for (inmatch::HostPlayerListRow &row : inmatch::host_player_list_rows(role_->state.host_owner.ctx)) {
		if (row.own) continue;
		out.push_back(std::move(row.slot));
	}
	return out;
}

// A ServerCommand runs on the match (inmatch::run_host_server_command, the game shell's path
// too). PuntPlayer at the server's own slot leaves the hosting (the
// session's word drops and the match's NovaWorld exit ends it). SetServerName / SetServerMsg /
// SetMPReset write the cfg block (game_name +0x3A5, servermsg +0x560, mpreset +0x344) beside the
// live config and save game.cfg, as retail's handler does, so the map change's re-apply of the
// block keeps them; the changed columns read back through registration() and ride the next
// refresh or the map change's republish. The executor already capped the strings to the block's
// widths (31 / 127).
// Before the match binds (the hosting wait, from the hosting request until the starting map
// creates the session) the command runs on a session-less context, as retail's statement
// dispatch hands every ServerCommand to the handler whatever the session's state: the connection
// mode is the dead /HOST path's host-only one and ctx+0x68 is clear, so every verb's gate refuses
// it but SetMPReset's token count, and a SetMPReset stores the word in the block and saves it,
// which the starting map's session create then reads. The context's is_in_session, the
// executor's stand-in for ctx+0x68, stays clear on purpose: retail's own +0x58 is already set
// in the hosting wait (CNapiNetwork_SetNetworkType @0x4C4A85, from @0x4A6614), but no verb's
// gate reads it.
// [orig: CNapiGameSession_HandleServerCommand — PuntPlayer @0x4D2515..0x4D254D; SetServerName's
//  block copy @0x4D2CFC, SetServerMsg's @0x4D2DAD, SetMPReset's @0x4D2E28; Game_SaveConfig
//  @0x4D2DDF (SetServerName / SetServerMsg) and @0x4D2E2D (SetMPReset); the gates, SetServerName's
//  ctx+0x68 test @0x4D2CE9 and SetMPReset's lone token count @0x4D2E12;
//  CNapiGameSession_DispatchServerStatement @0x4D18A0 (no state test, the name table
//  @0x82C0C8); Game_HostMultiplayerSession's SetConnectionMode(1) @0x4A661F ahead of its hosting
//  wait @0x4A6725..0x4A6735]
void ServeListing::on_command(const ServerCommand &command) {
	const bool bound = role_ != nullptr && kernel_ != nullptr;
	const inmatch::ServerCommandOutcome outcome =
			inmatch::run_host_server_command(bound ? &role_->state.host_owner.ctx : nullptr,
					bound ? &kernel_->world : nullptr, inmatch::host_connection_mode(false),
					seams_.config_block, server_command_verb_name(command.verb),
					server_command_target_name(command.target), command.args);
	if (!outcome.handled) {
		io::logf(LogLevel::kInfo, "[host] ServerCommand %s was not executed%s", command.command.c_str(),
		         bound ? "" : " (no session yet)");
		return;
	}
	io::logf(LogLevel::kInfo, "[host] ServerCommand %s", command.command.c_str());
	if (outcome.config_changed && seams_.save_config)
		seams_.save_config();
	if (outcome.stop_hosting && lister_ != nullptr) lister_->host_role().stop();
}

// [orig: CNapiGameSession_HandlePlayEnterResponse @0x4D1940]
void ServeListing::on_player_enter_result(const ClientSession::PlayerEnterResult &result) {
	if (role_ == nullptr) return;
	if (!inmatch::Server_ApplyPlayerEnterResult(role_->state.host_owner.ctx, result.connection_id,
	                                            result.success != 0, result.msg_code)) {
		io::logf(LogLevel::kDebug, "[host] ServerPlayerEnterResult for connection %u matched no held joiner",
		         result.connection_id);
	}
}

} // namespace opennova::serve
