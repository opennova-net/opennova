#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include <net/napi/session.h>          // SessionIdRing / ServerCommand
#include <net/novaworld/client_session.h>
#include <net/novaworld/gate_probe.h>
#include <net/npwire/net_ports.h>
#include <net/novaworld/lobby_vars.h>   // opennova::HostRegistration / HostLobbyText

#include "network/nwu_lobby_session.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace godot {

class RtxtStringFile;

// Godot-side NovaWorld HOST registration. The co-op/listen host (Simulation
// with enable_host_listen) is reachable on a game UDP port but, on a raw LAN
// join, the gate never learns about it and a retail client can't browse to it.
// This node makes the host appear in the gate's /api/hosts + the retail server
// browser by running the witnessed NWU lobby session to the gate (the same
// ClientHello -> ... -> Verified handshake NovaWorldClient runs) and then
// sending a ClientHostRequest, registered on the service's ServerHostResult
// and refreshed by ClientHostUpdate every SESSION_HOST_INFO_REFRESH_TICKS
// (the dirty delta) with ClientHostPlayerAdded/Removed per roster change. It is
// the host-direction sibling of NovaWorldClient (ADR 0010): sockets + signals
// here, protocol/crypto in engine/net/novaworld.
//
// [orig: CNapiGameSession_SendHostRequest @ 0x4d3700 / SendHostUpdate @ 0x4d3860 /
//  HandleHostVerifyResponse @ 0x4d59d0 / Server_TickUpdate @ 0x51d7e0 (the 0x744 refresh),
//  see docs/net/novaworld-net-re.md]
//
// Usage from GDScript:
//   var h := NovaWorldHost.new()
//   add_child(h)
//   h.host = "127.0.0.1"; h.gate_port = 7597
//   h.server_name = "Taylor's Game"; h.mission_name = "ASH_G11A"
//   h.game_port = 32768; h.max_players = 32
//   h.gametext = Strings.get_table(Strings.TABLE_GAMETEXT)
//   h.registered.connect(_on_registered)
//   h.start()
//   # as joiners arrive / leave:
//   h.set_player_slot(1, "Joiner", "192.168.1.5:32768", "", "1", "0")
//   h.clear_player_slot(1)
class NovaWorldHost : public Node {
	GDCLASS(NovaWorldHost, Node)

public:
	enum State {
		STATE_IDLE = 0,
		STATE_GATE_PROBING,   // gate probe sent, awaiting UDPNOVAWORLD
		STATE_SESSION_HELLO,  // ClientHello sent, handshake in flight
		STATE_SESSION_JOIN,   // ClientAuth/verify in flight
		STATE_REGISTERING,    // session Verified, ClientHostRequest sent (retail state 5)
		STATE_HOSTING,        // ServerHostResult Success: registered + refreshing (state 6)
		STATE_DISCONNECTED,
		STATE_ERROR,
	};

	NovaWorldHost();
	~NovaWorldHost();

	// Gate endpoint (the NovaWorld gate the launcher redirects to).
	void set_host(const String &host);
	String get_host() const;
	void set_gate_port(int port);
	int get_gate_port() const;

	// Lobby-visible host identity (the GSB row).
	void set_server_name(const String &name);
	String get_server_name() const;
	void set_server_message(const String &message);
	String get_server_message() const;
	void set_mission_name(const String &name);
	String get_mission_name() const;
	void set_game_type(const String &abbreviation);
	String get_game_type() const;
	void set_max_players(int n);
	int get_max_players() const;
	// The Region column selector: 0/1/2 -> the STRNOVA07/08/09 gametext tokens.
	void set_region_index(int index);
	int get_region_index() const;
	void set_player_name(const String &name);
	String get_player_name() const;
	void set_password(bool password);
	bool get_password() const;
	void set_listen_host(bool listen_host);
	bool get_listen_host() const;
	void set_locked(bool locked);
	bool get_locked() const;
	void set_allow_ping(bool allow_ping);
	bool get_allow_ping() const;
	void set_country(const String &country);
	String get_country() const;
	void set_expansion(const String &expansion);
	String get_expansion() const;
	void set_version(const String &version);
	String get_version() const;
	void set_time_of_day(int time_of_day);
	int get_time_of_day() const;
	void set_round_time_remaining_ticks(int ticks);
	int get_round_time_remaining_ticks() const;
	// The gametext table the Host list's STRNOVA/TimeOfDay tokens resolve through.
	void set_gametext(const Ref<RtxtStringFile> &gametext);
	Ref<RtxtStringFile> get_gametext() const;

	// The reachable game UDP endpoint joiners connect to (the Simulation
	// listen-host port). advertise_ip may be left empty to let the gate observe
	// the real source / apply its own ONNET_CLIENT_REFLECT_* override (the prod
	// path); set it when the host knows its own reachable address. The pair
	// rides the host's own ClientHostPlayerAdded (PlayerIpAndPort) — retail's
	// Host list carries no address and Port = "-1".
	void set_game_port(int port);
	int get_game_port() const;
	void set_advertise_ip(const String &ip);
	String get_advertise_ip() const;

	// LobbyName routed into HostSetup (gate lobby selection). The AppId is minted
	// per registration (a session random in [1000, 9999]); read it back here.
	void set_lobby_name(const String &lobby_name);
	String get_lobby_name() const;
	int get_app_id() const { return static_cast<int>(cfg_.app_id); }

	// Lifecycle.
	void start();
	void stop();

	State get_state() const { return state_; }
	bool is_hosting() const { return state_ == STATE_HOSTING; }

	// The roster the PlayerList and the ClientHostPlayerAdded/Removed statements
	// carry: one slot per player (slot 0 is the host itself, added at registration);
	// the five per-slot vars are the engine's HostPlayerSlot (lobby_vars.h).
	void set_player_slot(int slot, const String &player_name, const String &ip_and_port,
	                     const String &pcid, const String &team, const String &type);
	void clear_player_slot(int slot);
	// The Players column when the shell reports only an aggregate occupancy (no
	// roster slots): the count (clamped at 0) rides the next refresh and
	// overrides the roster size from then on.
	void set_player_count(int n);
	int get_player_count() const;

	// The GSID the service's ServerHostResult carried: the in-match host's 0x81 SUS1.
	String get_gsid() const;
	// True when the registration's ServerHostResult carried HostRequiresJoinTicket:
	// the in-match host then announces every validated joiner through
	// request_player_enter and holds it until player_enter_result. False before
	// registration and on a service that does not ask for tickets.
	bool get_host_requires_join_ticket() const;
	// ClientPlayerEnterRequest for a joiner the in-match host is validating: its
	// ConnectionId (dcb), its game endpoint (the inet_addr dword + port) and the
	// JOINTICKET its join carried (empty when the KV had none). The service answers
	// with ServerPlayerEnterResult (the player_enter_result signal). Only while
	// hosting is established. [orig: CNapiGameSession_SendPlayEnterRequest @0x4d02a0]
	void request_player_enter(int64_t connection_id, int64_t ip_address, int port,
	                          const String &join_ticket);
	// The retail error tag (NWECnn) of the last failure, or empty.
	String get_last_error_tag() const { return last_error_tag_; }

	// Engine hooks.
	void _ready() override;
	void _process(double delta) override;

protected:
	static void _bind_methods();

private:
	// The gate/session pump is the shared NwuLobbySession driver (lobby_);
	// the hooks map its progress onto our State + signals.
	NwuLobbySession::Hooks make_lobby_hooks();
	void sync_session_state();
	void drain_session_notices();  // ServerHostResult / Stop / punt / ServerCommand
	void send_host_request();      // ClientHostRequest (registration)
	void send_host_update(bool full); // ClientHostUpdate (full after registration, else the dirty delta)
	void send_status_blob();       // the plaintext POST heartbeat to the gate's POSTIPADDRESS:POSTIPPORT
	void enter_state(State next, const String &reason = String());

	// Gather the GDScript-set host config the libs builders consume.
	opennova::HostRegistration host_cfg() const;
	opennova::HostLobbyText lobby_text() const;
	std::vector<opennova::ClientVar> player_list_vars() const;

	// Config.
	String host_ = "127.0.0.1";
	int gate_port_ = opennova::GATE_DEFAULT_PORT;
	opennova::HostRegistration cfg_;   // the wire-facing columns
	String player_name_ = "Host";
	int game_port_ = opennova::kRetailLanPortMin;
	String advertise_ip_;
	Ref<RtxtStringFile> gametext_;

	// State.
	State state_ = STATE_IDLE;
	// The shared gate/session driver: sockets, ClientSession, ci/ck, the NW
	// endpoint, the gate auth-code stash, and the connect deadlines.
	NwuLobbySession lobby_;
	String last_error_tag_;
	std::map<int, opennova::HostPlayerSlot> players_;
	int player_count_override_ = -1;      // set_player_count; -1 = the roster size
	opennova::SessionIdRing pcid_ring_;
	// The dirty-delta baselines: what the last ClientHostUpdate carried, per list.
	std::vector<opennova::ClientVar> last_sent_host_;
	std::vector<opennova::ClientVar> last_sent_players_;
	double refresh_ticks_ = 0.0;          // logic ticks since the last refresh
	double uptime_s_ = 0.0;               // the Age column's clock (registration time)
	uint32_t register_started_ms_ = 0;    // the 60 s host poll
};

} // namespace godot

VARIANT_ENUM_CAST(godot::NovaWorldHost::State);
