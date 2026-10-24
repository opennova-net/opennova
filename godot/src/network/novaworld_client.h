#pragma once

#include <net/novaworld/gate_probe.h>
#include <godot_cpp/classes/http_request.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <net/novaworld/client_session.h>
#include <net/novaworld/http_flow.h>
#include <net/novaworld/lobby_vars.h>
#include <net/novaworld/nwu_host_role.h>
#include <net/novaworld/nwu_lobby_session.h>
#include <net/novaworld/proxy_rendezvous.h>

#include "network/novaworld_server_row.h"
#include "network/novaworld_gate_info.h"
#include "network/ping_sweep_worker.h"
#include "rtxt/rtxt_string_file.h"
#include "network/udp_pump.h"
#include "network/udp_pump_datagram_socket.h"
#include "player/game_config_block.h"
#include "resource_index/resource_root.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace opennova::inmatch {
class ConnectLog;
struct ServerLogs;
} // namespace opennova::inmatch

namespace godot {

class HostSessionOptions;
class JoinTarget;

// Godot-side novaworld client. One instance per game client.
// Wraps the C++ libs (engine/net/novacrypto + engine/net/napi + engine/net/novaworld) via
// GDExtension so GDScript can drive the full handshake without seeing the
// raw NWU/CRC/TLV machinery.
//
// Usage from GDScript:
//   var nw := NovaWorldClient.new()
//   add_child(nw)
//   nw.host = "127.0.0.1"
//   nw.gate_port = 7597
//   nw.player_name = "Taylor"
//   nw.connected.connect(_on_connected)
//   nw.disconnected.connect(_on_disconnected)
//   nw.start()
//
// State machine:
//   Idle -> GateProbing -> SessionHello -> SessionJoin -> Connected
//                                                       \-> Disconnected
//   Connected -> Joining -> InGameHello            (the play leg)
//   Connected -> HostingRequested -> Hosting       (the host leg)
//
// This node is retail's one NovaWorld game session (CNapiGameSession): the
// session the player logs in and browses with is the one that plays or hosts,
// and the shell keeps it up through the match (SessionDrive::adopt_nw_client).
class NovaWorldClient : public Node {
	GDCLASS(NovaWorldClient, Node)

public:
	enum State {
		STATE_IDLE = 0,
		STATE_GATE_PROBING,
		STATE_SESSION_HELLO,
		STATE_SESSION_JOIN,
		STATE_CONNECTED,
		STATE_DISCONNECTED,
		STATE_ERROR,
		// Join legs (ADD only at the end — existing values must stay stable for
		// GDScript). STATE_JOINING: NWJoin HTTP in flight, then the ClientPlayRequest
		// awaiting its ServerPlayResult. STATE_IN_GAME_HELLO: the in-match host:port has
		// been resolved and handed to the game layer (joined_game); Simulation's joiner
		// takes over the in-match handshake from here.
		STATE_JOINING,
		STATE_IN_GAME_HELLO,
		// The host leg: ClientHostRequest sent, awaiting its ServerHostResult
		// (the session's state 5), then hosting (state 6).
		STATE_HOSTING_REQUESTED,
		STATE_HOSTING,
	};

	NovaWorldClient();
	~NovaWorldClient();

	// Configurables (exposed as GDScript properties).
	void set_host(const String &host);
	String get_host() const;

	void set_gate_port(int port);
	int get_gate_port() const;

	void set_player_name(const String &name);
	String get_player_name() const;

	// The mounted game whose title the session speaks (its identity and its gate
	// tag, the title's or the mount's gt.ssc's; the launch game's with none,
	// ResourceRoot::session_title): start() refuses when the title's identity is
	// not witnessed in full.
	void set_resource_root(const Ref<ResourceRoot> &root) { resource_root_ = root; }
	Ref<ResourceRoot> get_resource_root() const { return resource_root_; }

	// The process's cfg block, whose mpnovaworld quad the session socket scans
	// (inmatch::transport_bind_ports) and whose mpgateserverlocalport quad the
	// gate socket scans (inmatch::gate_bind_ports); none scans the quads' shipped
	// defaults.
	void set_game_config(const Ref<GameConfigBlock> &p_block) { game_config_ = p_block; }
	Ref<GameConfigBlock> get_game_config() const { return game_config_; }

	// The gametext table the hosted Host list's STRNOVA / TimeOfDay tokens
	// resolve through (null keeps the stock English fallbacks).
	void set_gametext(const Ref<RtxtStringFile> &gametext);
	Ref<RtxtStringFile> get_gametext() const { return gametext_; }

	// Lifecycle.
	void start();
	void stop();
	// The player's ESC while the NovaWorld menu waits: the first connect's poll gives the connect
	// up with NWEC41 / NWEC47 (the error signal, a frame later), the play leg's poll leaves the
	// play with NWEC03 (join_failed), and the host leg's wait takes the key and does nothing (it
	// reads no key). False when none waits (engine: NwuLobbySession::escape; connect_or_host.h's
	// play lines).
	bool escape_connect();
	// ConnectOrHost's play or host leg awaits its result: the menu frame that runs it does not end
	// until the leg does, so the process pumps no remote admin meanwhile (GameWorld feeds this
	// into inmatch::RemoteAdminIdleFrame). Process-wide: the one NovaWorld session's.
	static bool process_connect_or_host_wait();
	// The process exit's teardown (a session create's mpreset exit): the
	// verified session's disconnect burst alone, with no ClientStopPlaying or
	// ClientStopHosting, as the exit's atexit teardown of the NovaWorld session
	// sends it (docs/net/novaworld-net-re.md D-NET-385; opennova-serve's
	// serve::Lister::disconnect). Nothing for a session not verified.
	void disconnect_at_exit();
	// The menu's teardown (an expansion reload): the process's kept gate response is
	// forgotten, so the next start() probes the gate again
	// (opennova::NwuLobbySession::GateMemory::forget, D-NET-706).
	static void forget_gate_response();
	// The menu's creation (the process's first main menu, and the menu made anew after a mods
	// reload): the process's menu browser state made anew, its jar emptied and loaded from
	// nw_cdata.coo in `dir`, the game's working directory, under the current volume's key
	// (engine: MenuBrowserState::create, cookie_jar_key).
	static void create_cookie_jar(const String &dir);
	// A save site of the shell (the menu's teardown on the way out or before a mods reload, the
	// NovaWorld screen's close, an external browser's open): the process's jar written to its file
	// (engine: MenuBrowserState::save; a failure is only reported, as retail drops it).
	static void save_cookie_jar();
	// The process's _connectlog.txt (the game world's log device, which `/CONNECTLOG` arms):
	// the NovaWorld gate and connect lines go to it (null: none).
	static void set_process_connect_log(opennova::inmatch::ConnectLog *log);
	// The same log, for the process's LAN browse (LanSession's session-record lines).
	static opennova::inmatch::ConnectLog *process_connect_log();
	// The process's log devices the host role's _inout.txt line goes to (null: none).
	static void set_process_server_logs(const opennova::inmatch::ServerLogs *logs);
	// The gate response's GLSVSSREQUEST (its wire bytes; empty without one), which a NovaWorld
	// join's 0x00 JOIN carries (JoinTarget.glsvss_request, D-NET-723).
	PackedByteArray get_glsvss_request() const;
	// The local player's name in this NovaWorld session, a join's and a host's alike: the menu
	// browser's CHAR cookie (engine: novaworld_character_name, the session apply's read) when
	// the jar holds a non-empty one, else `profile_name`, the profile's callsign.
	String get_session_player_name(const String &profile_name) const;

	State get_state() const { return state_; }
	bool is_session_active() const { return state_ == STATE_CONNECTED; }
	bool is_authenticated() const { return authenticated_; }
	// The NWU session as the match reads it (C++ only): the shell keeps this
	// node alive through the match (SessionDrive::adopt_nw_client).
	opennova::NwuLobbySession::MatchFacts nwu_match_facts() const { return lobby_.match_facts(); }
	// The session's periodic update at a mission load's loading step (C++ only), which holds the
	// frames: the session's own pump at the live clock. Its notices wait for the frame's drain
	// (engine: inmatch::ClientProcessState::nwu_periodic_update, D-NET-NEW-L9-25).
	void run_load_step_update();

	// Host a game (the NovaWorld menu's Host): ConnectOrHost's hosting leg on
	// this logged-in session -- the session-state and gate checks, the hosting
	// page's HOSTKEY (the main page's @HOST_URL@, which needs the login), the
	// host var lists and ClientHostRequest, then the 60 s wait for the
	// ServerHostResult.
	// Emits hosting_started once hosting (the shell then loads the mission) or
	// host_failed(NWEC tag). `options` supplies the registration columns.
	void start_hosting(const Ref<HostSessionOptions> &options);
	bool is_hosting() const { return host_role_.is_hosting(); }
	// The NovaWorld menu's re-entry after a match: CGameSession_StopHosting and
	// CGameSession_StopPlaying (each a statement only from its own states),
	// back to the verified lobby.
	void stop_hosting();
	void stop_playing();
	// The hosting session's in-match half (C++ only): the roster, the round
	// clock, the GSID, the join tickets and the ServerCommand config changes.
	opennova::NwuHostRole &host_role() { return host_role_; }
	// The NovaWorld network type's one socket: a hosting session's match rides
	// it (its demux's game view), so the service's observed NWU source is the
	// game endpoint (D-NET-346); a join's browse and connection ride it too
	// (D-NET-NEW-R3-3).
	Ref<UdpPump> get_game_pump() const { return nw_pump_; }
	// The gate reply the lobby HTTP legs resolve their base URL from; null
	// until a gate response landed.
	Ref<NovaWorldGateInfo> get_server_info() const;

	// Server browser (ADR 0010 Phase 2). The list is fetched over HTTP from
	// the GSB endpoint once the session is verified; rows arrive asynchronously
	// (watch the `server_list_updated` signal, then read get_server_rows()).
	TypedArray<NovaWorldServerRow> get_server_rows() const;

	// Re-fetch the server list on demand (the browser's Refresh button). A
	// no-op before the session/base URL exists; the fresh rows arrive through
	// the usual `server_list_updated` signal.
	void refresh_servers();

	// The GSB response's list-wide totals: the service-wide server and
	// player counts the retail browser shows as its population line (zeros
	// until the first list lands).
	int get_total_servers() const { return total_servers_; }
	int get_total_players() const { return total_players_; }

	// The ping sweep's per-row results so far: rid (int) -> ping. A
	// non-negative value is the echo round-trip in ms; -2 = failed/timed out,
	// -3 = never attempted (engine/net/novaworld/ping_sweep.h carries the
	// witnessed fold + timeout/retry constants). Rows still in flight are
	// absent. Repopulated per list refresh; `server_pings_updated` fires when
	// a pass lands.
	// The sweep results as two parallel packed arrays: the row ids and their
	// ping (ms, or -2 / -3 for the engine's unreachable / never-attempted
	// codes). A row absent from the arrays has no result yet.
	PackedInt64Array get_server_ping_rids() const;
	PackedInt32Array get_server_ping_values() const;

	// The NovaWorld menu's entry on a verified session (engine: LobbyHttpFlow::enter): the gate's
	// startup URL opened, its start page followed to NWStart, whose page is the lobby (a login the
	// service remembers: web_lobby_opened(true, nwhandle), then the list load) or the sign-in page
	// (web_lobby_opened(false, ""), whose Login posts that page's form); a failure is login_failed.
	// The first entry runs it itself once the session validates (the menu's state 9); the panel
	// runs it on a re-entry. A Login pressed while it is in flight posts once the sign-in page
	// lands.
	void open_web_lobby();

	// Account login (ADR 0010 Phase 3). Runs the EPASK HTTP login chain
	// (prepare GET -> login POST -> relay GET) and fills the cookie jar the
	// client carries onto every later request. Emits login_succeeded /
	// login_failed. Session-only (nothing persisted).
	void login(const String &username, const String &password);
	// The sign-in page's remembered login (engine: LobbyHttpFlow's remembered login): its name for
	// the account box, whether its fill stands, the clear a click on a sign-in field runs, the
	// unclicked Login's post (login_succeeded / login_failed as login's), the page shown again
	// after a failed login (its fill re-run) and the page's submit gate over the fields.
	String get_remembered_login_name() const;
	bool has_remembered_login() const;
	void clear_remembered_login();
	void login_remembered();
	void reshow_sign_in();
	bool is_sign_in_submit_enabled(const String &name, const String &password) const;
	// The sign-in page's REMEMBER_LOGIN checkbox (engine: LobbyHttpFlow::store_remember_login): its
	// state as the page's activation or a click left it (false before either), and a click's store,
	// `checked` the state after the click, into the browser state's property and the cfg block's
	// rememberlogin, which the shell's game.cfg save points write.
	bool is_remember_login_checked() const;
	void store_remember_login(bool checked);

	// Join a hosted game (ADR 0010 Phase 5). Runs the NWJoin.dll HTTP handshake for
	// the GSB row's `rid`, resolves the in-match host address, sends the
	// ClientPlayRequest (PlaySetup from the resolved .joi) and, once the service's
	// ServerPlayResult admits the play, emits joined_game(host, port, app_id,
	// cd_cookie). The game layer (NovaWorldPanel -> MainGame) then drives the
	// in-match join through Simulation's joiner — this client does not send the
	// in-match ClientHello itself (one joiner seam for LAN / NW / env joins).
	void join(int rid);

	// The proxy-assisted join of the last resolved .joi (NI/NP/BK next to NK), the
	// engine's ProxyRendezvousConfig (engine/net/novaworld/proxy_rendezvous.h):
	// has_join_proxy() is false when the join carried none; copy_join_proxy_to
	// hands it to the join target, whose in-match joiner sends the 48-byte
	// rendezvous on its enumerator cadence.
	bool has_join_proxy() const { return join_proxy_.enabled(); }
	void copy_join_proxy_to(const Ref<JoinTarget> &p_target) const;
	// The LN lobby number of the last resolved .joi (0 = none): nonzero means the
	// in-match dial targets the LAN-discovered endpoint instead of the NK relay,
	// and the play leg reports it as the "Lan" session var.
	int get_join_lobby_number() const { return pending_join_.ln; }
	// The login's PCID cookie ("" before a login): a NovaWorld host's own
	// player carries it as its PCID [orig: NapiNPServer_HandleNewConnection
	// @0x4c8288 reads the "PCID" cookie].
	std::string get_login_pcid() const;

	// Engine hooks.
	void _ready() override;
	void _process(double delta) override;

protected:
	static void _bind_methods();

private:
	// The gate/session pump is the shared NwuLobbySession driver (lobby_);
	// these hooks feed it the role-specific pieces and map its progress onto
	// our State + signals + trace ring.
	opennova::NwuLobbySession::Hooks make_lobby_hooks();
	opennova::NwuLobbySession::Environment make_lobby_environment();
	void on_gate_response(const opennova::GateResponse &parsed);
	std::vector<std::pair<std::string, std::string>> make_cookie_vars();
	void trace_sent_datagram(const opennova::PeerAddr &to, const std::vector<uint8_t> &dg);
	void on_session_datagram(const opennova::NwuLobbySession::RxInfo &rx);
	opennova::NwuHostRole::Hooks make_host_hooks();
	void sync_session_state(); // ClientSession::State -> our State + signals
	void drain_session_notices(); // the server notifications (stop/punt/command/results)

	// Server-browser HTTP leg. trigger_gsb() ships the GSB GET the flow builds; the
	// completion callback feeds the response back and caches the parsed rows.
	void trigger_gsb();
	void on_gsb_request_completed(int result, int response_code,
	                              const PackedStringArray &headers,
	                              const PackedByteArray &body);

	// The menu entry's HTTP chain (open_web_lobby) — sequenced by LobbyHttpFlow::on_enter_response.
	void on_enter_request_completed(int result, int response_code,
	                                const PackedStringArray &headers,
	                                const PackedByteArray &body);
	// Account login HTTP chain (ADR 0010 Phase 3) — sequenced by LobbyHttpFlow.
	// A login's first step out of the flow (login, login_remembered): ship it or report it.
	void begin_login(const opennova::LoginResult &r);
	void on_login_request_completed(int result, int response_code,
	                                const PackedStringArray &headers,
	                                const PackedByteArray &body);
	// Join HTTP chain (ADR 0010 Phase 5) — sequenced by LobbyHttpFlow.
	void on_join_request_completed(int result, int response_code,
	                               const PackedStringArray &headers,
	                               const PackedByteArray &body);
	// Host HTTP chain: the hosting page's HOSTKEY ahead of the host request.
	void on_host_request_completed(int result, int response_code,
	                               const PackedStringArray &headers,
	                               const PackedByteArray &body);
	// The hosting leg's HTTP part failed: back to the verified lobby, host_failed(reason).
	void fail_host_leg(const String &reason);
	// The host request's wait ended: the HOSTKEY page's refresh (pending_host_page_) goes out.
	void ship_host_page();
	// The NWJoin handshake resolved the host: send the ClientPlayRequest and wait for
	// the ServerPlayResult (the start-playing poll, 60 s).
	void start_playing(const opennova::JoinResult &resolved);
	// The service admitted the play: hand the in-match host:port to the game
	// layer via joined_game(host, port, app_id, cd_cookie). Does NOT send an
	// in-match hello; Simulation's joiner owns the single ClientHello (see the .cpp).
	void resolve_join_target();
	// The play poll's pass after the session's pump (engine: connect_or_host.h's
	// play_leg_exit_tag): the 60 s test, the key read, then the session's state; it ends the
	// leg on the result's state 8 (the join target) or any state but 7 (join_failed).
	void poll_play();

	// Snapshot the gate/session outputs into the flow's LobbyHttpContext. Called at
	// each leg-initiation point (login / GSB / join) — never inside a leg callback,
	// so a multi-step login keeps a stable http_base(). set_context preserves the
	// flow's shared cookie jar (NWHANDLE/PCID ride login -> GSB -> join).
	void sync_flow_context();
	// Ship one HttpRequestSpec over the given HTTPRequest child (method/url/headers/body).
	Error ship_spec(HTTPRequest *http, const opennova::HttpRequestSpec &spec);

	void enter_state(State next, const String &reason = String());

	// One wire/session trace line under --verbose; the lines keep the retail
	// _connectlog "SENDING N BYTES ... [0xNN]" shape so a capture diff lines up.
	void trace(const String &line);

	// Config.
	String host_ = "127.0.0.1";
	int gate_port_ = opennova::GATE_DEFAULT_PORT;
	String player_name_ = "GodotPlayer";

	Ref<RtxtStringFile> gametext_;
	Ref<ResourceRoot> resource_root_;
	Ref<GameConfigBlock> game_config_;

	// State.
	State state_ = STATE_IDLE;
	bool authenticated_ = false; // true only after the EPASK login returns NWHANDLE
	Ref<NovaWorldGateInfo> server_info_;
	// The driver's two sockets (bound at start(), closed at stop()) and their
	// engine adapters, and the driver's ms wall clock: the process's one GetTickCount clock
	// (engine world::frame_tick_count_ms) read at each process step, from its reading at
	// start() (the lobby's clock restarts at 0 with each open, NwuLobbySession::open).
	Ref<UdpPump> gate_pump_;
	Ref<UdpPump> nw_pump_;
	std::unique_ptr<UdpPumpDatagramSocket> gate_socket_;
	uint32_t clock_origin_ms_ = 0;
	uint32_t clock_ms_ = 0;
	// The process frame whose start() ran the probe (Engine::get_process_frames): the menu's
	// state-1 frame, which runs no lobby tick of its own (_process).
	uint64_t probe_frame_ = ~uint64_t{ 0 };
	// The shared gate/session driver (engine/net/novaworld): ClientSession, ci/ck,
	// the NW endpoint, and the connect deadlines all live in here.
	opennova::NwuLobbySession lobby_;
	// The hosting half of the session (declared after lobby_, which it drives).
	opennova::NwuHostRole host_role_;
	// The registration the hosting page's HOSTKEY completes.
	opennova::HostRegistration pending_host_cfg_;

	// The CD-key/hardware identity set (CountryName..NWHWI), built once per
	// session and used for BOTH the UDP verify var-list and the HTTP login
	// cookies. NWUID is left empty here and filled from the SessionInit at use.
	std::vector<std::pair<std::string, std::string>> identity_vars_;

	// Server browser.
	HTTPRequest *browser_http_ = nullptr;   // child node, created in start()
	std::vector<opennova::GsbServerEntry> server_entries_; // the cached GSB list
	bool gsb_request_in_flight_ = false;    // transport bookkeeping (cancel before re-issue)
	int total_servers_ = 0;                 // GSB TS — list-wide server count
	int total_players_ = 0;                 // GSB TP — service-wide player count
	// The browse-time ping sweep (retail pings every row's IPv4 on the list
	// finalize; the semantics live in engine/net/novaworld/ping_sweep.h, the
	// lifecycle in ping_sweep_runner.h and the device leg in
	// network/ping_sweep_worker.cpp). A refreshed list's sweep supersedes the
	// running one at once; the generation stamps each sweep so a deferred
	// result of a replaced list is dropped.
	Dictionary server_pings_;               // rid (int) -> ping ms / -2 / -3
	int64_t ping_generation_ = 0;
	PingSweepWorker ping_worker_;
	void start_ping_sweep();
	void apply_ping_reply(int64_t rid, int ms, int64_t generation);
	void apply_ping_results(const Dictionary &results, int64_t generation);

	// Account login + join (ADR 0010 Phase 3/5). Separate child HTTPRequests so
	// the multi-leg login/join sequences don't race the GSB fetch. The protocol/
	// sequencing/cookie-jar all live in flow_ (engine/net/novaworld); these are pure pumps.
	HTTPRequest *login_http_ = nullptr;
	HTTPRequest *enter_http_ = nullptr; // the menu entry's startup-URL leg (open_web_lobby)
	// A Login pressed while the entry's open was in flight: posted once the sign-in page lands.
	bool pending_login_ = false;
	String pending_login_user_, pending_login_pass_;
	HTTPRequest *join_http_ = nullptr;
	HTTPRequest *host_http_ = nullptr;
	// The HOSTKEY page's refresh GET, held until the host request's wait ends, and its answer due.
	opennova::HttpRequestSpec pending_host_page_;
	bool host_page_in_flight_ = false;
	// The lobby HTTP orchestration: the EPASK login chain, the GSB fetch, and the
	// NWJoin handshake (URL builders + shared cookie jar + LoginStep/JoinStep). The
	// binding ships each HttpRequestSpec and feeds (transport_ok, code, headers, body)
	// back in. Context is set from the gate/session legs via sync_flow_context().
	opennova::LobbyHttpFlow flow_;

	String nw_web_domain_;            // NovaworldWebDomainNameAndPortNumber from the
	                                  // SessionInit (real NW); the HTTP login/GSB/join
	                                  // host that replaces the startupurl [domainname]
	// The resolved join awaiting its ServerPlayResult (the start-playing poll):
	// the .joi endpoint + tokens, the proxy fields, and the poll's start time.
	opennova::JoinResult pending_join_;
	uint32_t pending_join_rid_ = 0;          // the browsed row -> PlaySetup ServerName
	std::string pending_join_server_name_;
	bool play_in_flight_ = false;
	uint32_t play_started_ms_ = 0;
	// The player's ESC the play poll reads at its next frame (escape_connect).
	bool play_escape_requested_ = false;
	// ConnectOrHost's play leg in flight, or the host leg's request awaiting its result.
	bool connect_or_host_wait() const;
	// The process-wide wait (process_connect_or_host_wait) from this client's legs; a client
	// going away (alive false) leaves it.
	void publish_connect_or_host_wait(bool alive = true);
	opennova::ProxyRendezvousConfig join_proxy_;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::NovaWorldClient::State);
