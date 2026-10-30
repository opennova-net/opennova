#pragma once

// THE JOIN FLOW: how a networked session starts from the front end, the shell-facing half of
// retail's join machine. A join target (a LAN row, a NovaWorld page's connect, a `--lan-join`
// launch) is enumerated for its flag word first when it has none, then asks the player/spectator
// question when the word offers a choice, then dials: the pre-dial expansion switch with its
// profile save, then the joiner load. ABORT_CANCEL and BACK run state 9, the session's full
// destroy and the main menu, a join entered from the NovaWorld menu going back there with the
// session it kept (g_ReturnToNovaWorldMenu). The launch flags' sessions (`--lan-host`,
// `--lan-join`) enter here too. The front end drives it and implements its device (the browse's
// socket, the join screen, the mount, the loads); every decision and every order is this file's.
// [orig: MultiPlayer_JoinSessionStateMachine @0x56a320 - state 3 UI_EnumerateAndJoinSession
//  called @0x56a499, state 4 UI_JoinSelectedSession called @0x56a4c9, state 8 (the choice), state
//  9 CNapiGameSession_FullDestroy(-1) called @0x56a3e3 then the main menu @0x56a3f9;
//  UI_EnumerateAndJoinSession @0x569f50; UI_JoinSelectedSession @0x5699d0]
//
// The flow's states and retail's:
//   Enumerating  state 3, the enumerate-and-join's browse, until a session answers or the abort
//   Choosing     state 8 (PRE_JOINSESSIONCHOICE), the choice panels (pre_game_menu.h)
//   Refused      the error arm of a join refused before its dial, Cancel only
//   LaunchSearch the `--lan-join browse` search (none of retail's: a LAN screen search run by flag)
//   Idle         no join in front of the dial (the dial hands the target to the joiner load,
//                whose own machine runs states 4..7: join_session_policy.h)

#include <base/resource_index/boot_policy.h> // LaunchFlags
#include <net/novaworld/proxy_rendezvous.h> // ProxyRendezvousConfig
#include <net/npwire/net_ports.h>
#include <runtime/inmatch/disconnect_reason.h> // ConnectionErrorRecord
#include <runtime/inmatch/join_role.h>
#include <runtime/inmatch/napi_np_server_ctx.h> // NetworkType
#include <runtime/inmatch/server_flags.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace opennova::inmatch {

// A join request (ADR 0017): the LAN row the player activated, the NovaWorld page's resolved join
// or the `--lan-join` flag, carried through the flow to the joiner load. Retail's enumeration
// supplies an ENDPOINT, not a map: the mission arrives post-auth in the S2C 0x7B session record,
// so `mission`, `server_name` and `game_type` are display hints only (D-NET-194).
struct JoinTargetRecord {
	std::string host_ip = "127.0.0.1";
	int port = kRetailLanPortMin;
	// The joiner's callsign; the dial fills the profile's when empty.
	std::string player_name;
	std::string mission;
	// Resource-dir override (dev and tests); empty = the persisted directory.
	std::string dir;
	// The game-session APPID join token a NovaWorld host validates (reject code 9); "0" on a LAN.
	std::string app_id = "0";
	// The CD identity cookie and the gate's GLSVSSREQUEST a NovaWorld JOIN carries (D-NET-723).
	std::vector<uint8_t> cd_cookie;
	std::vector<uint8_t> glsvss_request;
	std::string server_name;
	int game_type = -1;
	// ServerHello P2 (server_flags.h); -1 = not discovered yet.
	int32_t server_flags = -1;
	// The menu's connect type, retail's transport_mode on the client.
	NetworkType network_type = NetworkType::Lan;
	JoinRole join_role = JoinRole::Player;
	// The role question answered (the choice, or a launch flag).
	bool role_explicit = false;
	std::string spectator_password;
	std::string server_password;
	std::string join_password; // the side password
	// The host's expansion as the discovered session advertised it (the 0x81 SUS2, a NovaWorld
	// row's GSB Exp; "" the base game), meaningful only when `expansion_known`.
	std::string expansion;
	bool expansion_known = false;
	// The discovered session's record a join from it connects with: no 0x41 of its own, the 0x42
	// with the row's HK and SF and the browse's CI (JoinerConnection::DiscoveredSession, D-NET-521,
	// D-NET-850). False: a target the flow enumerates first (D-NET-703).
	bool discovered_session = false;
	int64_t session_host_key = 0;
	bool session_password_required = false;
	int64_t session_client_index = 0;
	// The proxy-assisted NovaWorld join (the .joi NI/NP/BK); disabled = none.
	ProxyRendezvousConfig join_proxy;
	// The .joi LN lobby number; nonzero dials the LAN-discovered endpoint, not the NK relay.
	int lobby_number = 0;
};

// What the flow knows of a target before dialing (server_flags.h join_entry_step's facts).
JoinEntryFacts join_entry_facts(const JoinTargetRecord &target);
inline JoinEntryStep join_entry_step(const JoinTargetRecord &target) {
	return join_entry_step(join_entry_facts(target));
}

// The choice's commit onto the target (pre_game_menu.h pre_game_commit): GAME_PASSWORD's text is
// the server password, SPECTATE the role (answered), and TEAM_PASSWORD's one shared text the
// spectator password when spectating, else the side password, the other kept.
void apply_pre_game_choice(JoinTargetRecord &target, const std::string &game_password,
		bool spectate, const std::string &team_password);

// One discovered session as the browse lists it (the 0x81 a session answered with): the endpoint
// it answered from and the ServerHello facts the join reads.
struct DiscoveredSessionRow {
	std::string host_ip;
	int port = 0;
	std::string server_name;
	int64_t game_type = -1;
	int64_t server_flags = -1;
	std::string expansion;
	int64_t host_key = 0;
	bool password_required = false;
	int64_t client_index = 0;
};

// A LAN row's target: its endpoint, its display hints, its flag word and expansion, and its
// session record (a join from the row sends no 0x41 of its own).
JoinTargetRecord join_target_from_row(const DiscoveredSessionRow &row);

// The enumerate-and-join's discovered session onto its target: the game type and the session
// name the record carries, its flag word and expansion, its session record, and the address it
// answered from (the join connects there, on the browse's socket).
// [orig: UI_EnumerateAndJoinSession @0x569f50 - g_GameType @0x56a207, the flags @0x56a20d /
//  @0x56a236, the name @0x56a218, the expansion @0x56a22b]
void apply_enumerated_row(JoinTargetRecord &target, const DiscoveredSessionRow &row);

// The enumerate's row: the first discovered session (index 0), whatever answered first.
// -1 with none. [orig: UI_EnumerateAndJoinSession @0x569f50 - g_SelectedSessionIndex = 0 @0x56a10d]
int enumerated_row(const std::vector<DiscoveredSessionRow> &rows);
// The `--lan-join browse[:<port>]` search's row: the first on `port` (any row when 0); -1 none.
int launch_search_row(const std::vector<DiscoveredSessionRow> &rows, int port);

// An OpenNova browser rule, not a retail witness: the server refused this build or install
// outright, so retrying the same server cannot work. NCC007 (the protocol version (PV2) gate),
// GCC003 (a banned address: the validate callback's JFP 3), and the join build (GDC002..008) and
// expansion (GDC047, its version.txt GDC048) refusals. Null (no connection) refuses nothing.
bool refuses_this_install(const ConnectionErrorRecord *record);

// The `--lan-host` launch's host request: what the flags set over the cfg block's session words
// (every other word is the block's, D-NET-335). The rotation holds the first mission and the
// `--lan-rotation` ones after it, each at launch option 0.
struct LaunchHost {
	std::string mission;
	std::vector<std::string> rotation; // empty: the mission alone
	int max_players = 4;
	int port = 0; // 0 = the block's first bind port
	int game_type = -1; // -1 = auto, from the mission (00TRg's zero mode: 0x10020)
	int lan_mode = 0; // 0 = the block's
};
std::optional<LaunchHost> launch_host(const LaunchFlags &flags);

enum class JoinFlowState {
	Idle,
	Enumerating,
	Choosing,
	Refused,
	LaunchSearch,
};

// The device the flow runs over: the front end's (the game's shell today).
class JoinFlowDevice {
public:
	virtual ~JoinFlowDevice() = default;

	// The browse: `target` set = the enumerate of its endpoint, unwindowed, under its network type
	// (a NovaWorld host admits only a NovaWorld browse), a NovaWorld join's on the held session's
	// socket with its proxy rendezvous ahead of each announce burst (D-NET-712); null = the
	// launch's search on the LAN screen's defaults. Its rows reach JoinFlow::browse_rows with
	// `serial`. False when it could not start.
	virtual bool begin_browse(const JoinTargetRecord *target, uint32_t serial) = 0;
	// The browse's row is the join's: its socket goes onto the join (D-NET-1019) and its records
	// to the process's one enumerator, where the join's session reset drains them (D-NET-956).
	virtual void adopt_browse() = 0;
	// The browse stops and goes.
	virtual void end_browse() = 0;
	// The browse's records drain, each writing its removed line.
	virtual void drain_browse() = 0;
	// The process enumerator (the LAN screen's browse) drains its records.
	virtual void drain_enumerator() = 0;

	// The join screen: the enumerate's PRE_ENUMSESSION line with ABORT_CANCEL; the choice
	// panels for `server_flags`; a refusal's text with Cancel only. Each Cancel or BACK reaches
	// JoinFlow::cancel. False when the screen cannot show it.
	virtual bool show_enumerating() = 0;
	virtual bool show_choice(uint32_t server_flags) = 0;
	virtual bool show_refusal(const std::string &text) = 0;
	virtual void close_join_screen() = 0;

	// The dial's legs: the profile's callsign; the mount's switch to `expansion` ("" when it
	// switched, else why not); the player profile's save and its reload from the mount; the
	// joiner load (the join screen until the host starts the game).
	virtual std::string callsign() = 0;
	virtual std::string switch_expansion(const std::string &expansion) = 0;
	virtual void save_profile() = 0;
	virtual void reload_profile() = 0;
	virtual void save_config() = 0;
	virtual void start_join(const JoinTargetRecord &target) = 0;
	// The `--lan-host` launch's listen host.
	virtual void start_launch_host(const LaunchHost &host) = 0;

	// State 9: the pre-dial session's destroy (the held server stopped and, off NovaWorld, the
	// transport torn down with its line), the game type's reset, the main menu, then the NovaWorld
	// session the destroy kept back to the NovaWorld menu (`reenter`) or stopped.
	virtual void destroy_pre_dial_session(bool novaworld) = 0;
	virtual void cancel_pre_game() = 0;
	virtual void show_main_menu() = 0;
	virtual void route_novaworld_session(bool reenter) = 0;
};

class JoinFlow {
public:
	explicit JoinFlow(JoinFlowDevice &device) : device_(device) {}
	JoinFlow(const JoinFlow &) = delete;
	JoinFlow &operator=(const JoinFlow &) = delete;

	// "Join this server": the target's entry step (server_flags.h join_entry_step) enumerates it,
	// asks the choice or dials. A join in front of the dial already (its browse, its screen) goes.
	void join(JoinTargetRecord target);
	// A LAN row the join machine refuses before it dials (its index names no listed session,
	// net/npwire/lan_discovery.h join_session): `text` (CVUNABLETOCREATE) with Cancel only.
	// [orig: UI_JoinSelectedSession @0x5699d0 - CVUNABLETOCREATE @0x569a78..0x569a89]
	void refuse(const std::string &text);
	// The NovaWorld page's connect URL resolved a join: the page's launch saves the profile, then
	// game.cfg, before it dials, and a normal exit from its match (or the join's abandon) returns
	// to the NovaWorld menu. `rid` is the listed server, whose install refusal the browser keeps.
	// [orig: UI_DispatchScreenEvent @0x54E6A0 - CNapiGameSession_ConnectOrHost called @0x54F33B;
	//  Menu_TeardownShellAndCloseBinkVideos @0x54E430 sets g_ReturnToNovaWorldMenu
	//  @0x54E4E3..0x54E508]
	void join_from_novaworld(JoinTargetRecord target, uint32_t rid);
	// The NovaWorld menu's host went into its match: a normal exit returns to the NovaWorld menu.
	void host_from_novaworld() { return_to_novaworld_ = true; }

	// The launch flags' session: `--lan-host` hosts, `--lan-join browse[:<port>]` searches and
	// joins the row found, `--lan-join <ip[:port]>` joins that endpoint (enumerated first, as every
	// target without a flag word is); `--spectator` / `--spectator-password` answer the role
	// question. False when the flags name none.
	bool launch(const LaunchFlags &flags);

	// The browse's rows (its `serial`'s; a stale browse's do nothing).
	void browse_rows(uint32_t serial, const std::vector<DiscoveredSessionRow> &rows);
	// The launch search's window ended with no row on its port.
	void browse_finished(uint32_t serial);
	// The choice's ACCEPT committed its three controls; the join dials.
	void commit_choice(
			const std::string &game_password, bool spectate, const std::string &team_password);
	// ABORT_CANCEL, BACK or Cancel: state 9 (the enumerate's, the choice's, the refusal's), or the
	// launch search's end. Nothing in Idle.
	void cancel();

	// A join that failed on the join screen: the NovaWorld server it went to, when the failure
	// refuses this install (refuses_this_install), whose browser row then reads unjoinable for the
	// rest of the process. Once per NovaWorld join.
	std::optional<uint32_t> join_failed(const ConnectionErrorRecord *record);
	// The post-mission route reads g_ReturnToNovaWorldMenu once: whether the menu it enters is the
	// NovaWorld menu, the flag cleared. [orig: Menu_InitShellResources @0x552500 - the return arm
	// @0x5526B3..0x5526C7 (UI_EnterNovaWorldMenu called @0x5526C2)]
	bool take_return_to_novaworld();
	// The held NovaWorld session went (the shell's teardown, a new hold over an old one).
	void release_novaworld() { return_to_novaworld_ = false; }

	JoinFlowState state() const { return state_; }
	bool choice_pending() const { return state_ == JoinFlowState::Choosing; }
	const JoinTargetRecord &target() const { return target_; }

private:
	void stop_browse();
	void begin_enumerate();
	void finish_enumerate(const DiscoveredSessionRow &row);
	void begin_choice();
	void dial();
	void abort_enumerate();
	void cancel_choice();
	void leave_abandoned_join();
	void apply_launch_role(JoinTargetRecord &target) const;

	JoinFlowDevice &device_;
	JoinFlowState state_ = JoinFlowState::Idle;
	JoinTargetRecord target_;
	uint32_t serial_ = 0;
	// The launch search's port (0 = any) and the launch's role answer.
	int launch_port_ = 0;
	bool launch_spectator_ = false;
	std::string launch_spectator_password_;
	// g_ReturnToNovaWorldMenu, and the NovaWorld join in flight (its listed server).
	bool return_to_novaworld_ = false;
	bool joining_novaworld_ = false;
	uint32_t joining_rid_ = 0;
};

} // namespace opennova::inmatch
