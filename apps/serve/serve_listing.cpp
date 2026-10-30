#include "serve_listing.h"

#include "lister.h"

#include <base/io/log.h>
#include <runtime/inmatch/host_config.h> // run_host_server_command
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_tick.h>

namespace opennova::serve {

using io::LogLevel;

// The match side of the listing is the engine's, the game's listen host's too
// (inmatch::HostListingBinding): the context's hooks into the hosting session and the second
// listing's (set_second), each role's columns off the live session at its server-info update. A
// map change binds again: the kernel is fresh, the server context kept.
void ServeListing::bind(inmatch::HostRole &role, mission::MissionKernel &kernel, Lister &lister) {
	role_ = &role;
	kernel_ = &kernel;
	lister_ = &lister;
	inmatch::NapiNPServerCtx *ctx = &role.state.host_owner.ctx;
	binding_.bind([ctx] { return ctx; }, lister.host_role(), seams_.gametext);
	sync_session();
}

void ServeListing::set_second(Lister *second) {
	binding_.set_second(second != nullptr ? &second->host_role() : nullptr);
	// The second listing's AppId and ticket arm reach the server context at once, and go at once
	// with it, so a joiner still carrying its AppId is refused 9.
	if (role_ != nullptr) binding_.sync_registration(role_->state.host_owner.ctx, std::string());
}

void ServeListing::unbind() {
	binding_.unbind();
	role_ = nullptr;
	kernel_ = nullptr;
	lister_ = nullptr;
}

// Each update: the hosting session's registration onto the match (a dedicated server has no
// loopback, so no login PCID of its own) and the NWU session's facts for the authority's NovaWorld
// exit (inmatch::HostListingBinding::sync_registration, inmatch::set_nwu_match_facts carry the
// witnesses); the cookie-key ring the CD cookies decrypt under is the match's own.
void ServeListing::sync_session() {
	if (role_ == nullptr || lister_ == nullptr) return;
	inmatch::NapiNPServerCtx &ctx = role_->state.host_owner.ctx;
	binding_.sync_registration(ctx, std::string());
	inmatch::set_nwu_match_facts(ctx, lister_->lobby().match_facts());
}

bool ServeListing::start(int &exit_code) {
	(void)exit_code;
	return true;
}

// The live match changes every frame (the round clock, the joiners): once bound, every pass hands
// the host role the columns (they ride the match's next 1860-tick refresh as its changed vars) and
// syncs the roster (ClientHostPlayerAdded / Removed go out at once, as Server_PlayerAdd sends
// them).
bool ServeListing::refresh(uint32_t now_ms, bool force) {
	(void)now_ms;
	return force || role_ != nullptr;
}

// The Host list's columns. HostSetup's MaxPlayers is the host file's cap, clamped by the lister
// to 1..65 for a dedicated host; the refreshed list's is the published cap less the dedicated
// slot, so neither counts the server's own slot. Dedicated is the STRNOVA11 text (listen_host
// false). Once the match binds, the session's columns are the live session's, which a
// ServerCommand or a map change edits (inmatch::HostListingBinding::read_columns, the game's too).
// [orig: CNapiGameSession_BuildHostVarLists @0x4d0b50; CNapiGameSession_ConnectOrHost
//  @0x4d5046..0x4d507b; Lobby_UpdateServerInfo @0x4fe8c0 (@0x4feb03..0x4feb0a,
//  @0x4febe2..0x4fec52)]
HostRegistration ServeListing::registration() const {
	HostRegistration r = base_;
	r.listen_host = false;
	(void)binding_.read_columns(r);
	return r;
}

// The match's PlayerList roster (inmatch::host_listing_roster, the game shell's too). A mode-1
// host builds no local connection, so its own slot 0 is never published.
// [orig: CNapiGameSession_CreateSession @0x4c9b73]
std::vector<HostPlayerSlot> ServeListing::wanted_roster(const std::map<int, HostPlayerSlot> &current) const {
	(void)current;
	if (role_ == nullptr) return {};
	return inmatch::host_listing_roster(role_->state.host_owner.ctx, /*own=*/nullptr);
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
// [orig: CNapiGameSession_HandleServerCommand — the PuntPlayer arm @0x4D2515..0x4D254D; SetServerName's
//  block copy @0x4D2CFC, SetServerMsg's @0x4D2DAD, SetMPReset's @0x4D2E28; Game_SaveConfig
//  @0x4D2DDF (SetServerName / SetServerMsg) and @0x4D2E2D (SetMPReset); the gates, SetServerName's
//  ctx+0x68 test @0x4D2CE9 and SetMPReset's lone token count @0x4D2E12;
//  CNapiGameSession_DispatchServerStatement @0x4D18A0 (no state test, the name table
//  @0x82C0C8); Game_HostMultiplayerSession's SetConnectionMode(1) @0x4A661F ahead of its hosting
//  wait @0x4A6725..0x4A6735]
void ServeListing::on_command(const ServerCommand &command) { (void)run_command(command, lister_); }

bool ServeListing::run_command(const ServerCommand &command, Lister *issuer) {
	const bool bound = role_ != nullptr && kernel_ != nullptr;
	const inmatch::ServerCommandOutcome outcome =
			inmatch::run_host_server_command(bound ? &role_->state.host_owner.ctx : nullptr,
					bound ? &kernel_->world : nullptr, inmatch::host_connection_mode(false),
					seams_.config_block, server_command_verb_name(command.verb),
					server_command_target_name(command.target), command.args);
	if (!outcome.handled) {
		io::logf(LogLevel::kInfo, "[host] ServerCommand %s was not executed%s", command.command.c_str(),
		         bound ? "" : " (no session yet)");
		return false;
	}
	io::logf(LogLevel::kInfo, "[host] ServerCommand %s", command.command.c_str());
	if (outcome.config_changed && seams_.save_config)
		seams_.save_config();
	if (!outcome.stop_hosting || issuer == nullptr) return false;
	issuer->host_role().stop();
	return true;
}

void ServeListing::on_player_enter_result(const ClientSession::PlayerEnterResult &result) {
	apply_player_enter_result(result, 0);
}

// [orig: CNapiGameSession_HandlePlayEnterResponse @0x4D1940]
void ServeListing::apply_player_enter_result(const ClientSession::PlayerEnterResult &result,
                                             uint8_t listing) {
	if (role_ == nullptr) return;
	// One session per listing (D-NET-1385, none of retail's): a result names a joiner of the
	// listing whose service answered, never one the other service sent.
	for (const inmatch::NapiNPConnection &conn : role_->state.host_owner.ctx.np_protocol.connection_list) {
		if (conn.type == inmatch::NapiNPConnection::kTypeServerSide &&
				conn.connection_id == result.connection_id && conn.novaworld_listing != listing) {
			io::logf(LogLevel::kDebug, "[host] ServerPlayerEnterResult for connection %u from the other "
			         "listing's service; ignored", result.connection_id);
			return;
		}
	}
	if (!inmatch::Server_ApplyPlayerEnterResult(role_->state.host_owner.ctx, result.connection_id,
	                                            result.success != 0, result.msg_code,
	                                            role_->state.host_owner.now_tick)) {
		io::logf(LogLevel::kDebug, "[host] ServerPlayerEnterResult for connection %u matched no held joiner",
		         result.connection_id);
	}
}

} // namespace opennova::serve
