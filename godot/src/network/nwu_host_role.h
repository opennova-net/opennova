#pragma once

// The hosting half of the NovaWorld UDP (NWU) game session. Retail runs ONE
// game session per process (CNapiGameSession, stru_B5FF50+0x90): the session
// the player logs in, browses and joins with is the one ClientHostRequest goes
// out on when they host, and it stays up through the hosted match. This is the
// role-specific state that session carries while hosting -- the registration
// config, the PlayerList roster, the Host-list refresh, and the host-direction
// notices -- held by NovaWorldClient (the session's node) beside its play leg.
// Sockets stay in NwuLobbySession; the protocol stays in engine/net/novaworld.
// Not a registered Godot class -- a plain member of NovaWorldClient.
//
// [orig: CNapiGameSession_ConnectOrHost @0x4d4f10 (the teamId != 0 leg);
//  CNapiGameSession_BuildHostVarLists @0x4d0b50; CNapiGameSession_SendHostRequest @0x4d3700;
//  CNapiGameSession_SendHostUpdate @0x4d3860; CNapiGameSession_HandleHostVerifyResponse
//  @0x4d59d0; Server_TickUpdate @0x51d7e0 (the 0x744 refresh); Server_PlayerAdd @0x51d3f0]

#include "network/nwu_lobby_session.h"
#include "rtxt/rtxt_string_file.h"

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <net/napi/session.h>          // SessionIdRing / ServerCommand
#include <net/novaworld/client_session.h>
#include <net/novaworld/lobby_vars.h>   // HostRegistration / HostLobbyText / HostPlayerSlot

#include <cstdint>
#include <functional>
#include <map>
#include <vector>

namespace godot {

class NwuHostRole {
public:
	// Where the hosting leg stands: retail's session states 5 (host requested)
	// and 6 (hosting), Idle otherwise.
	enum class Phase {
		Idle,
		Requested,
		Hosting,
	};

	struct Hooks {
		// ServerHostResult success: the session is hosting (state 6).
		std::function<void()> on_hosting;
		// The hosting leg failed: the NWEC tag (NWEC51 / NWEC52 / NWEC53..60).
		std::function<void(const String &)> on_failed;
		// The service stopped the hosting (ServerStopHosting): its message key.
		std::function<void(const String &)> on_stopped;
		// A ServerCommand: the verb name, the target selector ("", "ByIndex",
		// "ByIpAndPort", "ByName", "ByPCID") and the argument tokens.
		std::function<void(const String &, const String &, const PackedStringArray &)> on_command;
		// ServerPlayerEnterResult for a joiner the host announced.
		std::function<void(int64_t, int, int, const String &, const String &)> on_player_enter_result;
	};

	NwuHostRole(NwuLobbySession &lobby, Hooks hooks);

	// The hosting leg of ConnectOrHost, once its session-state and gate checks
	// passed: the per-registration AppId, the host var lists, ClientHostRequest
	// (CurrentlyHosting = 0) and the 60 s poll. False when the request could not
	// be built (NWEC51, reported through on_failed).
	bool request(const opennova::HostRegistration &cfg);
	// ClientStopHosting (states 5/6 back to 4), the NovaWorld menu's re-entry
	// after a match and the session's teardown. Clears the roster.
	void stop();
	// The 60 s host poll and, while hosting, the Host-list refresh on the logic
	// clock. Call every frame after the lobby pumped.
	void process(double delta);
	// Route one session notice. True when the notice was a host-direction one.
	bool handle_notice(const opennova::ClientSession::Notice &notice);

	Phase phase() const { return phase_; }
	bool is_hosting() const { return phase_ == Phase::Hosting; }

	// The roster the PlayerList and ClientHostPlayerAdded/Removed carry: one
	// slot per player the hosted match added, the host's own player included
	// (Server_PlayerAdd's five per-slot vars, lobby_vars.h HostPlayerSlot).
	void set_player_slot(const opennova::HostPlayerSlot &slot);
	void clear_player_slot(int slot);
	// The live round clock the TimeLeft column reads at every refresh (-1 =
	// untimed).
	void set_round_time_remaining_ticks(int ticks) { cfg_.round_time_remaining_ticks = ticks; }
	// A ServerCommand's config change: the columns ride the next refresh.
	void set_server_name(const std::string &name) { cfg_.server_name = name; }
	void set_server_message(const std::string &message) { cfg_.server_message = message; }
	// The gametext table the Host list's STRNOVA / TimeOfDay tokens resolve through.
	void set_gametext(const Ref<RtxtStringFile> &gametext);

	// The GSID the service's ServerHostResult carried: the in-match host's 0x81
	// SUS1. Empty again once the connection tears down (ClientSession::host_gsid).
	String gsid() const;
	// The registration's ServerHostResult asked for join tickets.
	bool requires_join_ticket() const;
	int app_id() const { return static_cast<int>(cfg_.app_id); }
	// The cookie-key table the in-match host decrypts the joiners' CD cookies
	// under: the ring whose current key the Host list advertises as PCIDKey.
	const opennova::SessionIdRing &cookie_keys() const { return pcid_ring_; }
	// ClientPlayerEnterRequest for a joiner the in-match host is validating.
	void request_player_enter(uint32_t connection_id, uint32_t ip_address, uint32_t port,
			const std::string &join_ticket);

private:
	opennova::HostRegistration host_cfg() const;
	opennova::HostLobbyText lobby_text() const;
	std::vector<opennova::ClientVar> player_list_vars() const;
	void send_host_request(uint32_t currently_hosting);
	void send_host_update(bool full);
	void send_status_blob();

	NwuLobbySession &lobby_;
	Hooks hooks_;
	opennova::HostRegistration cfg_;
	Ref<RtxtStringFile> gametext_;
	Phase phase_ = Phase::Idle;
	std::map<int, opennova::HostPlayerSlot> players_;
	opennova::SessionIdRing pcid_ring_;
	// The dirty-delta baselines: what the last ClientHostUpdate carried, per list.
	std::vector<opennova::ClientVar> last_sent_host_;
	std::vector<opennova::ClientVar> last_sent_players_;
	double refresh_ticks_ = 0.0;          // logic ticks since the last refresh
	double uptime_s_ = 0.0;               // the Age column's clock (registration time)
	uint32_t register_started_ms_ = 0;    // the 60 s host poll
};

} // namespace godot
