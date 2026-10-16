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

// What a roster sync does to the published roster: the slots to remove (gone, or now another
// player's: its connection, name, address or PCID differs, a leave and a re-join between two
// syncs included), the players to add, and the kept players whose PlayerTeam / PlayerType alone
// changed.
// [orig: Server_HandlePlayerDisconnect @0x51B5C0 — ClientHostPlayerRemoved @0x51b5f3;
//  Server_PlayerAdd @0x51cbc0 — ClientHostPlayerAdded @0x51d4b5]
struct RosterSync {
	std::vector<int> removed;
	std::vector<HostPlayerSlot> added;
	std::vector<HostPlayerSlot> vars;
};
RosterSync plan_roster_sync(const std::map<int, HostPlayerSlot> &current,
                            const std::vector<HostPlayerSlot> &wanted);

// The hosting half of the NovaWorld UDP (NWU) game session. Retail runs ONE game session per
// process: the session the player logs in, browses and joins with is the one ClientHostRequest
// goes out on when they host, and it stays up through the hosted match. This is the role-specific
// state that session carries while hosting: the registration config, the PlayerList roster, the
// Host-list refresh, and the host-direction notices. Sockets stay in NwuLobbySession; the
// protocol stays in ClientSession.
// [orig: CNapiGameSession_ConnectOrHost @0x4d4f10 (the teamId != 0 leg);
//  CNapiGameSession_BuildHostVarLists @0x4d0b50; CNapiGameSession_SendHostRequest @0x4d3700;
//  CNapiGameSession_SendHostUpdate @0x4d3860; CNapiGameSession_HandleHostVerifyResponse
//  @0x4d59d0; Server_TickUpdate @0x51d7e0 (the 0x744 refresh); Server_PlayerAdd @0x51d3f0;
//  Game_StartMission @0x5248c6..0x524903 (the mission start's update)]
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
	// The mission start's server-info update on a NovaWorld authority in session, outside the
	// 1860-tick timer: both lists marked changed, the server-info update, whose tail sends the
	// marked lists, then in state 6 the lists forced out once more (two ClientHostUpdates, each
	// with the whole Host list and PlayerList); the cookie-key ring is left alone. A fresh host's
	// first full Host list, its HostKey included, goes out here: its host result publishes
	// nothing (handle_host_result, D-NET-465). The embedder runs it at every mission start.
	// [orig: Game_StartMission @0x5248c6..0x524903 — the marks @0x5248e4 / @0x5248ef,
	//  Lobby_UpdateServerInfo @0x5248f5, CPlayerManager_RebuildLists(1) @0x524903]
	void publish_mission_start();
	// The server-info update outside the refresh and the mission start, the join lock's toggle's
	// (inmatch::Server_ToggleJoinLock through its embedder's hook): the Host list's columns into
	// the session's list, then in state 6 the lists' changed vars, and the status heartbeat where
	// no NWU session is in use. Nothing while the role is idle.
	// [orig: Lobby_UpdateServerInfo @0x4fe8c0, its tail CPlayerManager_RebuildLists(0) @0x4ff63a;
	//  Server_ToggleJoinLock's call @0x50d762]
	void update_server_info();
	// Route one session notice. True when the notice was a host-direction one.
	bool handle_notice(const ClientSession::Notice &notice);

	Phase phase() const { return phase_; }
	bool is_hosting() const { return phase_ == Phase::Hosting; }

	// The roster the PlayerList and ClientHostPlayerAdded/Removed carry: one slot per player the
	// hosted match added, the host's own player included (Server_PlayerAdd's five per-slot vars).
	void set_player_slot(const HostPlayerSlot &slot);
	void clear_player_slot(int slot);
	// A kept player's PlayerTeam and PlayerType, as the in-match handlers SetOrCreate them
	// (a team change, a loadout's accept, a weapon-slot list): the vars alone change in the
	// session's PlayerList at once, with no ClientHostPlayerAdded, and a changed one rides the
	// next update. A slot not in the roster is left alone.
	void set_player_vars(int slot, const std::string &team, const std::string &type);
	// The roster onto `wanted` (plan_roster_sync): the removals, then the adds, then the kept
	// players' vars.
	void sync_roster(const std::vector<HostPlayerSlot> &wanted);
	const std::map<int, HostPlayerSlot> &roster() const { return players_; }
	// The live session the server-info update reads its columns from at each run, over the
	// registration's (the embedder's match: its name and message, game type, mission, caps,
	// lock, rules and round clock, inmatch::read_session_listing_columns); the embedder installs
	// it once its match binds and clears it as the match goes. The source returns false when no
	// session is there to read (a map change's load, between one mission's match and the
	// next): the last live read then stands, as retail's globals keep their values across the
	// load, never the registration's. A read lands in the role's columns ahead of every request,
	// update and status heartbeat, and a changed column rides the next update as a changed var.
	// Null: the registration's columns stand.
	// [orig: Lobby_UpdateServerInfo @0x4fe8c0 — every column read off the session's globals at
	//  each run, the SetOrCreates @0x4fe911..0x4ff3ff]
	using ColumnSource = std::function<bool(HostRegistration &)>;
	void set_column_source(ColumnSource source) { column_source_ = std::move(source); }
	// Every listing column at once (opennova-serve's listing source, each pass its match
	// changed: Lister::refresh_listing): the registration's own keys stay (AppId, HostKey,
	// LobbyName, the PCIDKey ring, the ReconnectCounter) and the changed columns ride the next
	// refresh as its changed vars.
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
	// The column source's read into the role's columns: a successful one replaces them, a read
	// with no session leaves the last live columns standing.
	void read_columns();
	HostRegistration host_cfg() const;
	void send_host_request(uint32_t currently_hosting);
	void handle_host_result(const ClientSession::Notice &notice);
	// Both lists marked changed, the server-info update, then in state 6 the full lists and the
	// marks cleared: the block a mission start and a re-host's result share.
	void republish_lists();
	void send_status_blob();

	NwuLobbySession &lobby_;
	Hooks hooks_;
	HostRegistration cfg_;
	ColumnSource column_source_;
	HostLobbyText lobby_text_;
	Phase phase_ = Phase::Idle;
	std::map<int, HostPlayerSlot> players_;
	SessionIdRing pcid_ring_;
	double refresh_ticks_ = 0.0;           // logic ticks since the last refresh
	uint32_t last_tick_ms_ = 0;            // the previous tick's clock
	bool ticked_ = false;
	uint32_t hosting_started_ms_ = 0;      // the Age column's clock (registration time)
	uint32_t register_started_ms_ = 0;     // the 60 s host poll
};

} // namespace opennova
