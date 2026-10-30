// The NovaWorld host listing's match side (host_listing_binding.h).
#include <runtime/inmatch/host_listing_binding.h>

#include <net/novaworld/gate_metrics.h> // GateMetricsHost
#include <net/novaworld/nwu_host_role.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_info.h> // read_session_listing_columns, host_player_list_rows

#include <utility>

namespace opennova::inmatch {

HostListingBinding::~HostListingBinding() {
	ends_->first = nullptr;
	ends_->second = nullptr;
	ends_->match = nullptr;
}

void HostListingBinding::bind(
		MatchContext match, NwuHostRole &first, hud::GameTextLookup gametext) {
	if (bound()) unbind();
	ends_->match = std::move(match);
	ends_->first = &first;
	ends_->second = second_;
	ends_->gametext = std::move(gametext);
	const std::shared_ptr<Ends> ends = ends_;
	install_column_source(ends, first);
	if (second_ != nullptr) install_column_source(ends, *second_);
	NapiNPServerCtx *ctx = ends->match ? ends->match() : nullptr;
	if (ctx == nullptr) return;
	// A service that asked for join tickets arms the join-phase watchdog: each validating joiner is
	// announced as a ClientPlayerEnterRequest on the session of the listing whose service sent it
	// (its APPID that listing's, D-NET-1385), which issued its ticket, and held until the
	// ServerPlayerEnterResult answers or the ticket deadline reaps it.
	// [orig: CNapiNetwork_CheckPlayerTimeouts @0x4C8AD0 - the arm @0x4C8B88,
	//  CNapiGameSession_SendPlayEnterRequest @0x4C8BC1]
	ctx->on_player_enter_request = [ends](const NapiNPServerCtx::PlayerEnterRequest &request) {
		if (NwuHostRole *role = ends->listing(request.listing))
			role->request_player_enter(request.connection_id, request.peer.ip, request.peer.port,
					request.join_ticket);
	};
	// The match's server-info update (its 0x744 refresh in the server tick, the join lock's
	// toggle) runs the hosting session's, and the second listing's beside it (D-NET-1385, none of
	// retail's).
	// [orig: Server_TickUpdate @0x51D7E0 (the Lobby_UpdateServerInfo call @0x51D943);
	//  Server_ToggleJoinLock @0x50D750 -> Lobby_UpdateServerInfo(0) @0x50D762]
	ctx->on_update_server_info = [ends]() {
		if (ends->first != nullptr) ends->first->update_server_info();
		if (ends->second != nullptr) ends->second->update_server_info();
	};
	// A LAN-only host's admitted joiner's LANIP / LANPORT, on the listing that sent it
	// (D-GAMECFG-8). [orig: NapiNPServerMsg_0x002 @0x513146..0x51318C]
	ctx->on_lan_listing_address = [ends](uint8_t listing, uint32_t ip, uint32_t port) {
		if (NwuHostRole *role = ends->listing(listing)) role->set_lan_address(ip, port);
	};
	// The match's METPROTOCOL gate reports go out on the hosting session, their strings the
	// registration's and the gate reply's (gate_metrics_reports.h, D-NET-995).
	ctx->gate_metrics.host = [ends]() {
		return ends->first != nullptr ? ends->first->metrics_host() : GateMetricsHost{};
	};
	ctx->gate_metrics.game_text = ends->gametext;
	ctx->gate_metrics.send = [ends](const std::string &text) {
		if (ends->first != nullptr) ends->first->send_gate_metrics(text);
	};
}

void HostListingBinding::set_second(NwuHostRole *second) {
	if (second_ == second) return;
	if (bound()) {
		if (second_ != nullptr) second_->set_column_source(nullptr);
		if (second != nullptr) install_column_source(ends_, *second);
		ends_->second = second;
	}
	second_ = second;
}

void HostListingBinding::unbind() {
	if (!bound()) return;
	ends_->first->set_column_source(nullptr);
	if (ends_->second != nullptr) ends_->second->set_column_source(nullptr);
	if (NapiNPServerCtx *ctx = ends_->match ? ends_->match() : nullptr) {
		// The match stops announcing joiners to, refreshing and reporting on a session it is
		// leaving, and stops advertising its GSID.
		ctx->on_player_enter_request = nullptr;
		ctx->on_update_server_info = nullptr;
		ctx->on_lan_listing_address = nullptr;
		ctx->gate_metrics = GateMetricsSink{};
		ctx->novaworld_join_tickets_armed = false;
		ctx->novaworld_gsid.clear();
		ctx->novaworld_app_id = 0;
		ctx->novaworld_second_app_id = 0;
		ctx->novaworld_second_join_tickets_armed = false;
	}
	// A hook left on a context the match no longer resolves to (the game's simulation gone ahead of
	// the session) holds the old ends, which reach no role from here.
	ends_->first = nullptr;
	ends_->second = nullptr;
	ends_ = std::make_shared<Ends>();
}

// The update reads the match's columns as it runs: the server tick's refresh right after its
// cookie-key ring's advance, the round clock and the net-quality window as that tick left them
// (D-NET-1333); a map change's load holds no match, and the last read stands.
// [orig: Server_TickUpdate @0x51D7E0 - CSessionIdRing_AdvanceAndGenerate called @0x51D93D,
//  Lobby_UpdateServerInfo called @0x51D943; Lobby_UpdateServerInfo @0x4FE8C0 - the ring's current
//  key @0x4FF191..0x4FF196]
void HostListingBinding::install_column_source(
		const std::shared_ptr<Ends> &ends, NwuHostRole &role) {
	role.set_column_source([ends](HostRegistration &r) { return read_match_columns(*ends, r); });
}

bool HostListingBinding::read_match_columns(const Ends &ends, HostRegistration &r) {
	const NapiNPServerCtx *ctx = ends.first != nullptr && ends.match ? ends.match() : nullptr;
	if (ctx == nullptr) return false;
	read_session_listing_columns(r, *ctx, ends.gametext);
	return true;
}

bool HostListingBinding::read_columns(HostRegistration &r) const {
	return read_match_columns(*ends_, r);
}

void HostListingBinding::sync_registration(
		NapiNPServerCtx &ctx, const std::string &login_pcid) const {
	const NwuHostRole *first = ends_->first;
	if (first == nullptr) return;
	ctx.novaworld_gsid = first->gsid();
	ctx.novaworld_app_id = static_cast<uint32_t>(first->app_id());
	set_novaworld_account_facts(ctx, login_pcid);
	ctx.novaworld_join_tickets_armed = first->requires_join_ticket();
	const NwuHostRole *second = ends_->second;
	const bool second_hosting = second != nullptr && second->is_hosting();
	ctx.novaworld_second_app_id = second_hosting ? static_cast<uint32_t>(second->app_id()) : 0u;
	ctx.novaworld_second_join_tickets_armed = second_hosting && second->requires_join_ticket();
}

void set_nwu_match_facts(NapiNPServerCtx &ctx, const NwuLobbySession::MatchFacts &facts) {
	ctx.nwu_in_use = facts.in_use;
	ctx.nwu_session_role = facts.role;
	ctx.nwu_session_flags = static_cast<uint8_t>(facts.flags & 0xFFu);
	if (facts.exit_reason != 0) ctx.mission_exit_reason = facts.exit_reason;
}

std::vector<HostPlayerSlot> host_listing_roster(
		const NapiNPServerCtx &ctx, const HostPlayerSlot *own) {
	std::vector<HostPlayerSlot> out;
	for (HostPlayerListRow &row : host_player_list_rows(ctx)) {
		if (!row.own) {
			out.push_back(std::move(row.slot));
			continue;
		}
		if (own == nullptr) continue;
		HostPlayerSlot slot = *own;
		slot.team = row.slot.team;
		slot.type = row.slot.type;
		out.push_back(std::move(slot));
	}
	return out;
}

} // namespace opennova::inmatch
