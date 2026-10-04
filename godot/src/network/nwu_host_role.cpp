#include "network/nwu_host_role.h"

#include "network/random_id.h"
#include "rtxt/rtxt_string_file.h"
#include "util/string_convert.h"

#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <base/io/tick_rate.h>

#include <string>
#include <utility>
#include <vector>

namespace godot {

using opennova::to_gd;
using opennova::to_std;

NwuHostRole::NwuHostRole(NwuLobbySession &lobby, Hooks hooks) :
		lobby_(lobby), hooks_(std::move(hooks)) {}

void NwuHostRole::set_gametext(const Ref<RtxtStringFile> &gametext) { gametext_ = gametext; }

bool NwuHostRole::request(const opennova::HostRegistration &cfg) {
	cfg_ = cfg;
	players_.clear();
	last_sent_host_.clear();
	last_sent_players_.clear();
	pcid_ring_ = opennova::SessionIdRing{};
	refresh_ticks_ = 0.0;
	uptime_s_ = 0.0;
	// BuildHostVarLists mints the per-registration AppId first: the engine's
	// make_session_app_id (tick + rand folded into [1000, 9999]).
	cfg_.app_id = opennova::make_session_app_id(lobby_.clock_ms(),
			static_cast<int>(pick_random_uint32() & 0x7FFFu));
	// A fresh host sends CurrentlyHosting = 0 (ConnectOrHost's StartHostingSession(ctx, 0)).
	send_host_request(/*currently_hosting=*/0);
	if (phase_ != Phase::Requested) {
		if (hooks_.on_failed) hooks_.on_failed(String(opennova::NWEC_HOST_START_FAILED));
		return false;
	}
	return true;
}

void NwuHostRole::stop() {
	opennova::ClientSession *session = lobby_.session();
	if (phase_ != Phase::Idle && session != nullptr && lobby_.sockets_open()) {
		// CGameSession_StopHosting: states 5/6 back to 4 + the statement.
		session->stop_hosting();
	}
	phase_ = Phase::Idle;
	players_.clear();
}

void NwuHostRole::process(double delta) {
	opennova::ClientSession *session = lobby_.session();
	if (session == nullptr) return;
	if (phase_ == Phase::Requested) {
		// The host poll: no ServerHostResult within SESSION_CONNECT_TIMEOUT_MS ->
		// ClientStopHosting + NWEC52 (net/napi/session.h NWEC_HOST_TIMEOUT).
		if (lobby_.clock_ms() - register_started_ms_ > opennova::SESSION_CONNECT_TIMEOUT_MS) {
			stop();
			if (hooks_.on_failed) hooks_.on_failed(String(opennova::NWEC_HOST_TIMEOUT));
		}
		return;
	}
	if (phase_ != Phase::Hosting) return;
	uptime_s_ += delta;
	// The server-info refresh runs on the logic clock: every
	// SESSION_HOST_INFO_REFRESH_TICKS the cookie-key ring (SessionIdRing) advances
	// and the Host list is republished as the dirty delta.
	refresh_ticks_ += delta * opennova::io::kTickHz;
	if (refresh_ticks_ >= static_cast<double>(opennova::SESSION_HOST_INFO_REFRESH_TICKS)) {
		refresh_ticks_ -= static_cast<double>(opennova::SESSION_HOST_INFO_REFRESH_TICKS);
		pcid_ring_.advance(lobby_.clock_ms(), static_cast<int>(pick_random_uint32() & 0x7FFFu));
		cfg_.pcid_key = pcid_ring_.current();
		send_host_update(/*full=*/false);
		send_status_blob();
	}
}

// The host-direction notices (ServerHostResult, ServerStopHosting, ServerCommand,
// ServerPlayerEnterResult: the client msginfo rows ClientSession dispatches,
// engine/net/novaworld/session).
bool NwuHostRole::handle_notice(const opennova::ClientSession::Notice &notice) {
	using Notice = opennova::ClientSession::Notice;
	opennova::ClientSession *session = lobby_.session();
	switch (notice.kind) {
	case Notice::Kind::Rehost:
		// A reconnect's re-verify re-sent the hosting request itself
		// (CurrentlyHosting = 1); the hosting stands while its answer is out.
		return true;
	case Notice::Kind::HostResult:
		if (phase_ == Phase::Hosting) {
			// A re-host's answer: a refusal ends the hosting (the session's word
			// drops, and the hosted match exits on it).
			if (!notice.fields.success) phase_ = Phase::Idle;
			return true;
		}
		if (phase_ != Phase::Requested) return true;
		if (notice.fields.success) {
			// Hosting (state 6): the full Host list republishes at once and the
			// refresh clock starts.
			phase_ = Phase::Hosting;
			refresh_ticks_ = 0.0;
			uptime_s_ = 0.0;
			send_host_update(/*full=*/true);
			if (session != nullptr) {
				for (const auto &entry : players_) {
					session->send_host_player_added(entry.second);
				}
			}
			UtilityFunctions::print_verbose(String("[NovaWorld] hosting '") +
					to_gd(cfg_.server_name) + "' gsid=" + gsid() + " -> gate " +
					lobby_.nw_udp_host() + ":" +
					String::num_int64(static_cast<int64_t>(lobby_.nw_udp_port())));
			if (hooks_.on_hosting) hooks_.on_hosting();
		} else {
			// Rejected: the MsgCode maps through the host switch (NWEC53..60).
			phase_ = Phase::Idle;
			if (hooks_.on_failed) {
				hooks_.on_failed(to_gd(opennova::novaworld_host_error_tag(notice.fields.msg_code)));
			}
		}
		return true;
	case Notice::Kind::StopHosting:
		if (phase_ != Phase::Idle) {
			phase_ = Phase::Idle;
			if (hooks_.on_stopped) hooks_.on_stopped(to_gd(notice.msg_key));
		}
		return true;
	case Notice::Kind::Command: {
		// The verb's arguments are the service's wire bytes.
		PackedStringArray args;
		for (const std::string &arg : notice.command.args) args.push_back(opennova::cp1252_to_gd(arg));
		const char *target = "";
		switch (notice.command.target) {
		case opennova::ServerCommandTarget::ByIndex: target = "ByIndex"; break;
		case opennova::ServerCommandTarget::ByIpAndPort: target = "ByIpAndPort"; break;
		case opennova::ServerCommandTarget::ByName: target = "ByName"; break;
		case opennova::ServerCommandTarget::ByPCID: target = "ByPCID"; break;
		default: break;
		}
		if (hooks_.on_command) {
			hooks_.on_command(String(opennova::server_command_verb_name(notice.command.verb)),
					String(target), args);
		}
		return true;
	}
	case Notice::Kind::PlayerEnterResult:
		if (hooks_.on_player_enter_result) {
			hooks_.on_player_enter_result(static_cast<int64_t>(notice.player_enter.connection_id),
					notice.player_enter.success, notice.player_enter.msg_code,
					opennova::cp1252_to_gd(notice.player_enter.player_ticket),
					opennova::cp1252_to_gd(notice.player_enter.access_code_list));
		}
		return true;
	default:
		return false;
	}
}

// A changed column is dirty in the Host var-list and rides the next refresh (the
// dirty delta): retail republishes on its 1860-tick timer, not on the edit.
void NwuHostRole::set_player_slot(const opennova::HostPlayerSlot &slot) {
	players_[slot.slot] = slot;
	cfg_.player_count = static_cast<int>(players_.size());
	// ClientHostPlayerAdded fires immediately while hosting is established (state 6).
	opennova::ClientSession *session = lobby_.session();
	if (phase_ == Phase::Hosting && session != nullptr) {
		session->send_host_player_added(slot);
	}
}

void NwuHostRole::clear_player_slot(int slot) {
	if (players_.erase(slot) == 0) return;
	cfg_.player_count = static_cast<int>(players_.size());
	// ClientHostPlayerRemoved fires immediately while hosting is established (state 6).
	opennova::ClientSession *session = lobby_.session();
	if (phase_ == Phase::Hosting && session != nullptr) {
		session->send_host_player_removed(slot);
	}
}

// The GSID is the service's wire bytes (ServerHostResult HostCommands).
String NwuHostRole::gsid() const {
	const opennova::ClientSession *session = lobby_.session();
	return session ? opennova::cp1252_to_gd(session->host_gsid()) : String();
}

bool NwuHostRole::requires_join_ticket() const {
	const opennova::ClientSession *session = lobby_.session();
	return session != nullptr && session->host_requires_join_ticket() != 0;
}

void NwuHostRole::request_player_enter(uint32_t connection_id, uint32_t ip_address, uint32_t port,
		const std::string &join_ticket) {
	opennova::ClientSession *session = lobby_.session();
	if (session == nullptr) return;
	session->send_player_enter_request(connection_id, ip_address, port, join_ticket);
}

void NwuHostRole::send_status_blob() {
	const opennova::GateResponse &gate = lobby_.gate_response();
	std::vector<opennova::HostPlayerSlot> roster;
	for (const auto &entry : players_) roster.push_back(entry.second);
	const std::vector<uint8_t> packet = opennova::lobby_update_build_datagram(
			gate, opennova::make_host_status_blob(host_cfg(), lobby_text(), roster));
	if (packet.empty()) return;
	const String post_host = String::num_int64(gate.post_ip[0]) + "." + String::num_int64(gate.post_ip[1]) +
			"." + String::num_int64(gate.post_ip[2]) + "." + String::num_int64(gate.post_ip[3]);
	lobby_.send_to(post_host, static_cast<int>(gate.post_port), packet);
}

opennova::HostLobbyText NwuHostRole::lobby_text() const {
	opennova::HostLobbyText text;
	if (gametext_.is_null()) return text;
	auto lookup = [this](const char *section, const char *key, std::string &out) {
		if (gametext_->has_string_in_section(section, StringName(key)))
			out = to_std(gametext_->get_string_in_section(section, StringName(key)));
	};
	lookup("NovaWorld", "STRNOVA11", text.yes);
	lookup("NovaWorld", "STRNOVA12", text.no);
	lookup("NovaWorld", "STRNOVA10", text.no_time_limit);
	lookup("NovaWorld", "STRNOVA07", text.region[0]);
	lookup("NovaWorld", "STRNOVA08", text.region[1]);
	lookup("NovaWorld", "STRNOVA09", text.region[2]);
	// GameText_GetStringWithFallback("TimeOfDay", KEY, fallback): the fallback stands
	// when the table lacks the key.
	lookup("TimeOfDay", "UNKNOWN", text.time_of_day[0]);
	lookup("TimeOfDay", "DAWN", text.time_of_day[1]);
	lookup("TimeOfDay", "DAY", text.time_of_day[2]);
	lookup("TimeOfDay", "DUSK", text.time_of_day[3]);
	lookup("TimeOfDay", "NIGHT", text.time_of_day[4]);
	return text;
}

opennova::HostRegistration NwuHostRole::host_cfg() const {
	opennova::HostRegistration cfg = cfg_;
	cfg.player_count = players_.empty() ? 1 : static_cast<int>(players_.size());
	cfg.pcid_key = pcid_ring_.current();
	cfg.uptime_ms = static_cast<uint32_t>(uptime_s_ * 1000.0);
	return cfg;
}

std::vector<opennova::ClientVar> NwuHostRole::player_list_vars() const {
	std::vector<opennova::HostPlayerSlot> roster;
	for (const auto &entry : players_) roster.push_back(entry.second);
	return opennova::make_player_list(roster);
}

// [orig: CNapiGameSession_SendHostRequest @0x4d3700, see docs/net/novaworld-net-re.md]
void NwuHostRole::send_host_request(uint32_t currently_hosting) {
	opennova::ClientSession *session = lobby_.session();
	if (session == nullptr || !session->is_verified()) return;
	if (!session->request_hosting(host_cfg(), currently_hosting)) return;
	register_started_ms_ = lobby_.clock_ms();
	phase_ = Phase::Requested;
}

void NwuHostRole::send_host_update(bool full) {
	opennova::ClientSession *session = lobby_.session();
	if (session == nullptr || !session->is_verified()) return;
	// includeAll right after registration, else only the vars whose value changed
	// (dirty_client_vars), and nothing at all when none did.
	// [orig: CNapiGameSession_SendHostUpdate @0x4d3860, see docs/net/novaworld-net-re.md]
	const std::vector<opennova::ClientVar> host =
			opennova::make_host_var_list(host_cfg(), lobby_text(), /*full=*/true);
	const std::vector<opennova::ClientVar> players = player_list_vars();
	const std::vector<opennova::ClientVar> dirty_host =
			full ? host : opennova::dirty_client_vars(last_sent_host_, host);
	const std::vector<opennova::ClientVar> dirty_players =
			full ? players : opennova::dirty_client_vars(last_sent_players_, players);
	if (dirty_host.empty() && dirty_players.empty()) return;
	session->send_host_update(dirty_host, dirty_players);
	last_sent_host_ = host;
	last_sent_players_ = players;
}

} // namespace godot
