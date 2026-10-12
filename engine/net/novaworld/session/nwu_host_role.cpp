#include <net/novaworld/nwu_host_role.h>

#include <base/io/log.h>
#include <base/io/tick_rate.h>
#include <net/novaworld/lobby_update.h>
#include <net/npwire/peer_addr.h>

#include <string>
#include <utility>
#include <vector>

namespace opennova {

NwuHostRole::NwuHostRole(NwuLobbySession &lobby, Hooks hooks)
	: lobby_(lobby), hooks_(std::move(hooks)) {}

bool NwuHostRole::request(const HostRegistration &cfg) {
	cfg_ = cfg;
	players_.clear();
	last_sent_host_.clear();
	last_sent_players_.clear();
	pcid_ring_ = SessionIdRing{};
	refresh_ticks_ = 0.0;
	// BuildHostVarLists mints the per-registration AppId first: (tick + rand) folded into
	// [1000, 9999]. [orig: CNapiGameSession_BuildHostVarLists @0x4d0b6e ->
	//  CNapiNetwork_RandomizeTimeout @0x4c4d80]
	cfg_.app_id = make_session_app_id(lobby_.clock_ms(), static_cast<int>(lobby_.random_u32() & 0x7FFFu));
	// A fresh host sends CurrentlyHosting = 0 (ConnectOrHost's StartHostingSession(ctx, 0)).
	send_host_request(/*currently_hosting=*/0);
	if (phase_ != Phase::Requested) {
		if (hooks_.on_failed) hooks_.on_failed(NWEC_HOST_START_FAILED);
		return false;
	}
	return true;
}

void NwuHostRole::stop() {
	ClientSession *session = lobby_.session();
	// CGameSession_StopHosting: states 5/6 back to 4 + the statement.
	if (phase_ != Phase::Idle && session != nullptr && lobby_.is_open()) session->stop_hosting();
	phase_ = Phase::Idle;
	players_.clear();
}

void NwuHostRole::tick(uint32_t now_ms) {
	const uint32_t elapsed_ms = ticked_ ? now_ms - last_tick_ms_ : 0u;
	last_tick_ms_ = now_ms;
	ticked_ = true;
	ClientSession *session = lobby_.session();
	if (session == nullptr) return;
	if (phase_ == Phase::Requested) {
		// The host poll: no ServerHostResult within SESSION_CONNECT_TIMEOUT_MS ->
		// ClientStopHosting + NWEC52. [orig: CNapiGameSession_ConnectOrHost @0x4d4f10 (0xEA60)]
		if (now_ms - register_started_ms_ > SESSION_CONNECT_TIMEOUT_MS) {
			stop();
			if (hooks_.on_failed) hooks_.on_failed(NWEC_HOST_TIMEOUT);
		}
		return;
	}
	if (phase_ != Phase::Hosting) return;
	// The server-info refresh runs on the logic clock: every SESSION_HOST_INFO_REFRESH_TICKS the
	// cookie-key ring advances and the Host list is republished as the dirty delta.
	// [orig: Server_TickUpdate @0x51d7e0 (the 0x744 refresh) -> Lobby_UpdateServerInfo @0x4fe8c0]
	refresh_ticks_ += static_cast<double>(elapsed_ms) * io::kTickHz / 1000.0;
	if (refresh_ticks_ >= static_cast<double>(SESSION_HOST_INFO_REFRESH_TICKS)) {
		refresh_ticks_ -= static_cast<double>(SESSION_HOST_INFO_REFRESH_TICKS);
		pcid_ring_.advance(now_ms, static_cast<int>(lobby_.random_u32() & 0x7FFFu));
		cfg_.pcid_key = pcid_ring_.current();
		update_server_info();
	}
}

void NwuHostRole::update_server_info() {
	if (phase_ != Phase::Hosting) return;
	send_host_update(/*full=*/false);
	send_status_blob();
}

// The mission start runs its update only on a NovaWorld authority in session; while the request
// is out the marks stay and the first refresh in state 6 carries the whole lists.
// [orig: Game_StartMission @0x5248c6..0x5248dc (the NovaWorld session gates)]
void NwuHostRole::publish_mission_start() {
	if (phase_ == Phase::Idle)
		return;
	republish_lists();
}

// Retail marks every var of both lists changed, runs the server-info update (whose tail,
// CPlayerManager_RebuildLists(0), sends the marked lists: the whole of both), then in state 6
// sends both lists in full once more and clears the marks. Ours mark by emptying the dirty-delta
// baselines, against which every var is dirty.
// [orig: Game_StartMission — the marks on the Host list (session+0x214) @0x5248e4 and the
//  PlayerList (+0x25c) @0x5248ef, Lobby_UpdateServerInfo(0) @0x5248f5,
//  CPlayerManager_RebuildLists @0x4d4590 called with 1 @0x524903: the state-6 test @0x4d4593,
//  CNapiGameSession_SendHostUpdate @0x4d45b5, the marks cleared @0x4d45c2 / @0x4d45cf.
//  CNapiGameSession_HandleHostVerifyResponse runs the same block: the marks @0x4d5c63 /
//  @0x4d5c71, Lobby_UpdateServerInfo(0) @0x4d5c78, the state-6 test @0x4d5c80..0x4d5c87,
//  CNapiGameSession_SendHostUpdate(1) @0x4d5c8c, the marks cleared @0x4d5c95 / @0x4d5c9e]
void NwuHostRole::republish_lists() {
	last_sent_host_.clear();
	last_sent_players_.clear();
	update_server_info();
	// The state-6 test is send_host_update's, as it is for every send.
	send_host_update(/*full=*/true);
}

// The host-direction notices (ServerHostResult, ServerStopHosting, ServerCommand,
// ServerPlayerEnterResult: the client msginfo rows ClientSession dispatches).
bool NwuHostRole::handle_notice(const ClientSession::Notice &notice) {
	using Notice = ClientSession::Notice;
	if (notice.kind == Notice::Kind::HostResult) {
		handle_host_result(notice);
		return true;
	}
	switch (notice.kind) {
	case Notice::Kind::Rehost:
		// A reconnect's re-verify re-sent the hosting request itself (CurrentlyHosting = 1);
		// the hosting stands while its answer is out.
		return true;
	case Notice::Kind::StopHosting:
		if (phase_ != Phase::Idle) {
			phase_ = Phase::Idle;
			if (hooks_.on_stopped) hooks_.on_stopped(notice.msg_key);
		}
		return true;
	case Notice::Kind::Command:
		if (hooks_.on_command) hooks_.on_command(notice.command);
		return true;
	case Notice::Kind::PlayerEnterResult:
		if (hooks_.on_player_enter_result) hooks_.on_player_enter_result(notice.player_enter);
		return true;
	default:
		return false;
	}
}

// ServerHostResult: a fresh host's (the request out) or a re-host's after the NWU reconnect (the
// hosting standing). The success republishes both lists only while ReconnectCounter
// (session+0x500) is above 0: a re-host, whose re-verify counted it up. A fresh host's request
// zeroed it, so a fresh host publishes nothing here, and the players added while its request was
// out ride the next full PlayerList; its first full Host list, HostKey included, is its
// embedder's mission start's (publish_mission_start).
// [orig: CNapiGameSession_HandleHostVerifyResponse @0x4d59d0 — state 6 @0x4d5c1b, the gate
//  `cmp [esi+0x500], ebp; jle` @0x4d5c52..0x4d5c58 (ebp zeroed @0x4d5acd), the republish
//  @0x4d5c5a..0x4d5c9e; the counter's two writers: CNapiGameSession_BuildHostVarLists @0x4d0b50
//  (the zero @0x4d0b7b), CNapiGameSession_HandleConnectVerifyResponse @0x4d5800 (the count
//  @0x4d5968)]
void NwuHostRole::handle_host_result(const ClientSession::Notice &notice) {
	bool fresh = false;
	if (phase_ == Phase::Hosting) {
		// A re-host's answer: a refusal ends the hosting (the session's word drops, and the
		// hosted match exits on it).
		if (!notice.fields.success) {
			phase_ = Phase::Idle;
			return;
		}
	} else if (phase_ != Phase::Requested) {
		return;
	} else if (!notice.fields.success) {
		// Rejected: the MsgCode maps through the host switch (NWEC53..60).
		phase_ = Phase::Idle;
		if (hooks_.on_failed)
			hooks_.on_failed(novaworld_host_error_tag(notice.fields.msg_code));
		return;
	} else {
		// Hosting (state 6): the refresh clock starts.
		phase_ = Phase::Hosting;
		refresh_ticks_ = 0.0;
		hosting_started_ms_ = lobby_.clock_ms();
		fresh = true;
		io::logf(io::LogLevel::kInfo, "[NovaWorld] hosting '%s' gsid=%s -> %s",
				cfg_.server_name.c_str(), gsid().c_str(),
				peer_addr_to_string(lobby_.nw_peer()).c_str());
	}
	const ClientSession *session = lobby_.session();
	if (session != nullptr && session->host_reconnect_counter() > 0)
		republish_lists();
	if (fresh && hooks_.on_hosting)
		hooks_.on_hosting();
}

// A changed column is dirty in the Host var-list and rides the next refresh (the dirty delta):
// retail republishes on its 1860-tick timer, not on the edit.
void NwuHostRole::set_player_slot(const HostPlayerSlot &slot) {
	players_[slot.slot] = slot;
	cfg_.player_count = static_cast<int>(players_.size());
	// ClientHostPlayerAdded fires immediately while hosting is established (state 6). Retail then
	// sends the PlayerList delta at once as well, ours at the next refresh (D-NET-490).
	// [orig: Server_PlayerAdd @0x51d3f0 @0x51d421..0x51d4b5]
	ClientSession *session = lobby_.session();
	if (phase_ == Phase::Hosting && session != nullptr) session->send_host_player_added(slot);
}

void NwuHostRole::clear_player_slot(int slot) {
	if (players_.erase(slot) == 0) return;
	cfg_.player_count = static_cast<int>(players_.size());
	// ClientHostPlayerRemoved fires immediately while hosting is established (state 6).
	ClientSession *session = lobby_.session();
	if (phase_ == Phase::Hosting && session != nullptr) session->send_host_player_removed(slot);
}

void NwuHostRole::set_columns(const HostRegistration &columns) {
	HostRegistration next = columns;
	next.app_id = cfg_.app_id;
	next.host_key = cfg_.host_key;
	next.lobby_name = cfg_.lobby_name;
	next.pcid_key = cfg_.pcid_key;
	next.reconnect_counter = cfg_.reconnect_counter;
	next.player_count = cfg_.player_count;
	next.round_time_remaining_ticks = columns.round_time_remaining_ticks;
	cfg_ = next;
}

std::string NwuHostRole::gsid() const {
	const ClientSession *session = lobby_.session();
	return session ? session->host_gsid() : std::string();
}

bool NwuHostRole::requires_join_ticket() const {
	const ClientSession *session = lobby_.session();
	return session != nullptr && session->host_requires_join_ticket() != 0;
}

void NwuHostRole::request_player_enter(uint32_t connection_id, uint32_t ip_address, uint32_t port,
                                       const std::string &join_ticket) {
	ClientSession *session = lobby_.session();
	if (session == nullptr) return;
	session->send_player_enter_request(connection_id, ip_address, port, join_ticket);
}

// The plaintext status heartbeat to the gate's POSTIPADDRESS:POSTIPPORT goes out only when no NWU
// session is in use: with a UDPNOVAWORLD address named, the refresh rides ClientHostUpdate alone.
// [orig: Lobby_UpdateServerInfo @0x4fe8c0 — `cmp dword_B5FD2C, 0; jnz` @0x4ff441..0x4ff448 (the
//  NWU-in-use word, CNapiGateManager_ProcessResponse @0x4cf525..0x4cf541), then both POST fields
//  @0x4ff45c..0x4ff466 and CNapiNetwork_SendUDPPacket @0x4ff62c]
void NwuHostRole::send_status_blob() {
	if (lobby_.match_facts().in_use) return;
	const GateResponse &gate = lobby_.gate_response();
	std::vector<HostPlayerSlot> roster;
	for (const auto &entry : players_) roster.push_back(entry.second);
	const std::vector<uint8_t> packet = lobby_update_build_datagram(
			gate, make_host_status_blob(host_cfg(), lobby_text_, roster));
	if (packet.empty()) return;
	lobby_.send_to(peer_addr_from_octets(gate.post_ip, static_cast<uint16_t>(gate.post_port)), packet);
}

HostRegistration NwuHostRole::host_cfg() const {
	HostRegistration cfg = cfg_;
	// Players counts the active slots, the host's own only when it is a peer: a listen host's
	// slot 0 is active from the round init, a dedicated server's never counts, so its roster
	// (the joiners) is the whole count. [orig: Lobby_UpdateServerInfo @0x4feaa6..0x4feacb;
	//  Server_InitNewRoundState @0x51c99a..0x51ca3c keeps slot 0 active and local]
	const int roster = static_cast<int>(players_.size());
	cfg.player_count = cfg.listen_host && players_.count(0) == 0 ? roster + 1 : roster;
	// The CountryName / Lang / TZB trio follows the gate reply's METEXT (D-NET-347).
	// [orig: CNapiGateManager_ProcessResponse @0x4cf2a1..0x4cf2c1 -> g_IsDedicatedServer]
	cfg.met_ext = lobby_.gate_response().met_ext != 0;
	cfg.pcid_key = pcid_ring_.current();
	cfg.uptime_ms = phase_ == Phase::Hosting ? lobby_.clock_ms() - hosting_started_ms_ : 0u;
	return cfg;
}

std::vector<ClientVar> NwuHostRole::player_list_vars() const {
	std::vector<HostPlayerSlot> roster;
	for (const auto &entry : players_) roster.push_back(entry.second);
	return make_player_list(roster);
}

// [orig: CNapiGameSession_SendHostRequest @0x4d3700]
void NwuHostRole::send_host_request(uint32_t currently_hosting) {
	ClientSession *session = lobby_.session();
	if (session == nullptr || !session->is_verified()) return;
	if (!session->request_hosting(host_cfg(), static_cast<int>(currently_hosting))) return;
	register_started_ms_ = lobby_.clock_ms();
	phase_ = Phase::Requested;
}

// Every update goes in state 6 alone: CPlayerManager_RebuildLists tests it ahead of its send (the
// refresh's, the mission start's, the server-info update's tail) and the host result's includeAll
// send has its own test, so with a re-host's request out (state 5) no update leaves until its
// result. includeAll at a mission start and a re-host's result, else only the vars whose value
// changed (dirty_client_vars), and nothing at all when none did.
// [orig: CPlayerManager_RebuildLists @0x4d4590 — `cmp [esi+0x11c], 6` @0x4d4593;
//  CNapiGameSession_HandleHostVerifyResponse @0x4d59d0 (the test @0x4d5c80..0x4d5c87);
//  CNapiGameSession_SendHostUpdate @0x4d3860]
void NwuHostRole::send_host_update(bool full) {
	ClientSession *session = lobby_.session();
	if (session == nullptr || !session->is_verified() ||
			session->host_state() != ClientSession::HostState::Established)
		return;
	const std::vector<ClientVar> host = make_host_var_list(host_cfg(), lobby_text_, /*full=*/true);
	const std::vector<ClientVar> players = player_list_vars();
	const std::vector<ClientVar> dirty_host = full ? host : dirty_client_vars(last_sent_host_, host);
	const std::vector<ClientVar> dirty_players =
			full ? players : dirty_client_vars(last_sent_players_, players);
	if (dirty_host.empty() && dirty_players.empty()) return;
	session->send_host_update(dirty_host, dirty_players);
	last_sent_host_ = host;
	last_sent_players_ = players;
}

} // namespace opennova
