#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <net/napi/session.h>          // SessionIdRing / ServerCommand
#include <net/novaworld/client_session.h>
#include <net/novaworld/lobby_vars.h>   // HostRegistration / HostLobbyText / HostPlayerSlot
#include <net/novaworld/nwu_lobby_session.h>

namespace opennova {

// The hosting half of the NovaWorld UDP (NWU) game session. Retail runs ONE game session per
// process: the session the player logs in, browses and joins with is the one ClientHostRequest
// goes out on when they host, and it stays up through the hosted match. This is the role-specific
// state that session carries while hosting: the registration config, the PlayerList roster, the
// Host-list refresh, and the host-direction notices. Sockets stay in NwuLobbySession; the
// protocol stays in ClientSession.
// [orig: CNapiGameSession_ConnectOrHost @0x4d4f10 (the teamId != 0 leg);
//  CNapiGameSession_BuildHostVarLists @0x4d0b50; CNapiGameSession_SendHostRequest @0x4d3700;
//  CNapiGameSession_SendHostUpdate @0x4d3860; CNapiGameSession_HandleHostVerifyResponse
//  @0x4d59d0; Server_TickUpdate @0x51d7e0 (the 0x744 refresh); Server_PlayerAdd @0x51d3f0]
class NwuHostRole {
public:
	// Where the hosting leg stands: retail's session states 5 (host requested) and 6 (hosting),
	// Idle otherwise.
	enum class Phase {
		Idle,
		Requested,
		Hosting,
	};

	struct Hooks {
		// ServerHostResult success: the session is hosting (state 6).
		std::function<void()> on_hosting;
		// The hosting leg failed: the NWEC tag (NWEC51 / NWEC52 / NWEC53..60).
		std::function<void(const std::string &)> on_failed;
		// The service stopped the hosting (ServerStopHosting): its message key.
		std::function<void(const std::string &)> on_stopped;
		// A ServerCommand for the hosted match.
		std::function<void(const ServerCommand &)> on_command;
		// ServerPlayerEnterResult for a joiner the host announced.
		std::function<void(const ClientSession::PlayerEnterResult &)> on_player_enter_result;
	};

	NwuHostRole(NwuLobbySession &lobby, Hooks hooks);

	// The hosting leg of ConnectOrHost, once its session-state and gate checks passed
	// (host_leg_refusal, connect_or_host.h): the per-registration AppId, the host var lists,
	// ClientHostRequest (CurrentlyHosting = 0) and the 60 s poll. False when the request could
	// not be queued (NWEC51, reported through on_failed).
	bool request(const HostRegistration &cfg);
	// ClientStopHosting (states 5/6 back to 4), the NovaWorld menu's re-entry after a match and
	// the session's teardown. Clears the roster.
	void stop();
	// The 60 s host poll and, while hosting, the Host-list refresh on the logic clock. Call every
	// tick after the lobby's tick, with the same clock.
	void tick(uint32_t now_ms);
	// The server-info update outside the 1860-tick timer: the Host list's dirty delta (and the
	// status heartbeat where no NWU session is in use) now, the cookie-key ring left alone. A
	// mission start on a NovaWorld authority in session runs it.
	// [orig: Game_StartMission @0x5248f5 -> Lobby_UpdateServerInfo @0x4fe8c0 ->
	//  CPlayerManager_RebuildLists @0x4d45b5 -> CNapiGameSession_SendHostUpdate]
	void update_server_info();
	// Route one session notice. True when the notice was a host-direction one.
	bool handle_notice(const ClientSession::Notice &notice);

	Phase phase() const { return phase_; }
	bool is_hosting() const { return phase_ == Phase::Hosting; }

	// The roster the PlayerList and ClientHostPlayerAdded/Removed carry: one slot per player the
	// hosted match added, the host's own player included (Server_PlayerAdd's five per-slot vars).
	void set_player_slot(const HostPlayerSlot &slot);
	void clear_player_slot(int slot);
	const std::map<int, HostPlayerSlot> &roster() const { return players_; }
	// The live round clock the TimeLeft column reads at every refresh (-1 = untimed).
	void set_round_time_remaining_ticks(int ticks) { cfg_.round_time_remaining_ticks = ticks; }
	// A ServerCommand's config change: the columns ride the next refresh.
	void set_server_name(const std::string &name) { cfg_.server_name = name; }
	void set_server_message(const std::string &message) { cfg_.server_message = message; }
	// Every listing column at once (a mirrored server's new map, name, settings): the
	// registration's own keys stay (AppId, HostKey, LobbyName, the PCIDKey ring, the
	// ReconnectCounter) and the changed columns ride the next refresh as its dirty delta.
	void set_columns(const HostRegistration &columns);
	// The gametext strings the Host list's STRNOVA / TimeOfDay tokens resolve to.
	void set_lobby_text(const HostLobbyText &text) { lobby_text_ = text; }

	// The GSID the service's ServerHostResult carried: the in-match host's 0x81 SUS1. Empty
	// again once the connection tears down (ClientSession::host_gsid).
	std::string gsid() const;
	// The registration's ServerHostResult asked for join tickets.
	bool requires_join_ticket() const;
	int app_id() const { return static_cast<int>(cfg_.app_id); }
	// The cookie-key table the in-match host decrypts the joiners' CD cookies under: the ring
	// whose current key the Host list advertises as PCIDKey.
	const SessionIdRing &cookie_keys() const { return pcid_ring_; }
	// ClientPlayerEnterRequest for a joiner the in-match host is validating.
	void request_player_enter(uint32_t connection_id, uint32_t ip_address, uint32_t port,
	                          const std::string &join_ticket);

private:
	HostRegistration host_cfg() const;
	std::vector<ClientVar> player_list_vars() const;
	void send_host_request(uint32_t currently_hosting);
	void send_host_update(bool full);
	void send_status_blob();

	NwuLobbySession &lobby_;
	Hooks hooks_;
	HostRegistration cfg_;
	HostLobbyText lobby_text_;
	Phase phase_ = Phase::Idle;
	std::map<int, HostPlayerSlot> players_;
	SessionIdRing pcid_ring_;
	// The dirty-delta baselines: what the last ClientHostUpdate carried, per list.
	std::vector<ClientVar> last_sent_host_;
	std::vector<ClientVar> last_sent_players_;
	double refresh_ticks_ = 0.0;           // logic ticks since the last refresh
	uint32_t last_tick_ms_ = 0;            // the previous tick's clock
	bool ticked_ = false;
	uint32_t hosting_started_ms_ = 0;      // the Age column's clock (registration time)
	uint32_t register_started_ms_ = 0;     // the 60 s host poll
};

} // namespace opennova
