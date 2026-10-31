// The join flow (join_flow.h).
#include <runtime/inmatch/join_flow.h>

#include <base/io/log.h>
#include <runtime/inmatch/pre_game_menu.h> // pre_game_commit

#include <utility>

namespace opennova::inmatch {

JoinEntryFacts join_entry_facts(const JoinTargetRecord &target) {
	JoinEntryFacts facts;
	facts.server_flags = target.server_flags;
	facts.role_explicit = target.role_explicit;
	facts.spectator = target.join_role == JoinRole::Spectator;
	facts.server_password_given = !target.server_password.empty();
	facts.side_password_given = !target.join_password.empty();
	return facts;
}

void apply_pre_game_choice(JoinTargetRecord &target, const std::string &game_password,
		bool spectate, const std::string &team_password) {
	PreGameChoice choice;
	choice.side_password = target.join_password;
	choice.spectator_password = target.spectator_password;
	pre_game_commit(game_password, spectate, team_password, choice);
	target.server_password = choice.server_password;
	target.join_password = choice.side_password;
	target.spectator_password = choice.spectator_password;
	target.join_role = choice.spectate ? JoinRole::Spectator : JoinRole::Player;
	target.role_explicit = true;
}

JoinTargetRecord join_target_from_row(const DiscoveredSessionRow &row) {
	JoinTargetRecord target;
	target.host_ip = row.host_ip;
	target.port = row.port;
	target.server_name = row.server_name;
	target.game_type = static_cast<int>(row.game_type);
	target.server_flags = static_cast<int32_t>(row.server_flags);
	// The row's 0x81 SUS2 is the host's expansion: the join switches to it before it connects.
	target.expansion = row.expansion;
	target.expansion_known = true;
	// The row's record already holds the 0x81: the join sends no 0x41 of its own, and goes on
	// with the browse's connection and its CI (D-NET-521).
	target.discovered_session = true;
	target.session_host_key = row.host_key;
	target.session_password_required = row.password_required;
	target.session_client_index = row.client_index;
	return target;
}

void apply_enumerated_row(JoinTargetRecord &target, const DiscoveredSessionRow &row) {
	// The record's game type and session name are the join's from here (the loading screen's
	// hints until the post-auth 0x7B), its flag word the choice's, its SUS2 the expansion the dial
	// switches to.
	// [orig: UI_EnumerateAndJoinSession @0x569f50 - g_GameType @0x56a207, the name @0x56a218,
	//  the expansion @0x56a22b, g_PreGameServerFlags @0x56a236]
	target.game_type = static_cast<int>(row.game_type);
	target.server_name = row.server_name;
	target.server_flags = static_cast<int32_t>(row.server_flags);
	target.expansion = row.expansion;
	target.expansion_known = true;
	// The row is the join's discovered session: the join goes on with the browse's connection,
	// straight to the 0x42 with the row's HK and SF and the browse's CI, so the only 0x41s are
	// the browse's (D-NET-521; JoinerConnection::DiscoveredSession carries the witness), to the
	// address the session answered from (JoinerConnection::set_host_address).
	target.discovered_session = true;
	target.session_host_key = row.host_key;
	target.session_password_required = row.password_required;
	target.session_client_index = row.client_index;
	target.host_ip = row.host_ip;
	target.port = row.port;
}

int enumerated_row(const std::vector<DiscoveredSessionRow> &rows) {
	return rows.empty() ? -1 : 0;
}

int launch_search_row(const std::vector<DiscoveredSessionRow> &rows, int port) {
	for (size_t i = 0; i < rows.size(); ++i)
		if (port == 0 || rows[i].port == port)
			return static_cast<int>(i);
	return -1;
}

bool refuses_this_install(const ConnectionErrorRecord *record) {
	if (record == nullptr)
		return false;
	if (record->connect_error == 7)
		return true;
	if (record->connect_error == 14)
		return record->connect_param == 3;
	if (record->connect_error != 0 || record->disconnect_code != 2)
		return false;
	const uint32_t dpc = record->disconnect_param;
	return (dpc >= 2 && dpc <= 8) || dpc == 47 || dpc == 48;
}

std::optional<LaunchHost> launch_host(const LaunchFlags &flags) {
	if (flags.lan_host.empty())
		return std::nullopt;
	LaunchHost host;
	host.mission = flags.lan_host;
	// `--lan-rotation`: the missions after it, appended in order as the host screen's START
	// appends its selected rows, each at launch option 0.
	// [orig: UI_HandleHostSessionStart @0x556D88..0x556E2E; MissionRotation_Append @0x5019D0]
	if (!flags.lan_rotation.empty()) {
		host.rotation.push_back(flags.lan_host);
		host.rotation.insert(
				host.rotation.end(), flags.lan_rotation.begin(), flags.lan_rotation.end());
	}
	host.max_players = launch_lan_max_players(flags, host.max_players);
	host.port = launch_lan_port(flags, 0);
	// The session's g_GameType the host config chooses at the host start; an absent override is
	// `auto`, derived from the loaded mission [orig: Game_HostMultiplayerSession @0x4a65a0 -
	// g_GameType = the session's game type setting @0x4a6657].
	host.game_type = flags.lan_gametype;
	host.lan_mode = launch_lan_mode(flags, 0);
	return host;
}

void JoinFlow::stop_browse() {
	++serial_;
	device_.end_browse();
	device_.close_join_screen();
}

void JoinFlow::join(JoinTargetRecord target) {
	stop_browse();
	state_ = JoinFlowState::Idle;
	target_ = std::move(target);
	// The entry's step (server_flags.h join_entry_step): enumerate the endpoint for its flag word
	// first (every target without one), dial, or ask the player/spectator question.
	switch (join_entry_step(target_)) {
		case JoinEntryStep::Preflight:
			begin_enumerate();
			break;
		case JoinEntryStep::Prompt:
			begin_choice();
			break;
		case JoinEntryStep::Dial:
			dial();
			break;
	}
}

void JoinFlow::refuse(const std::string &text) {
	stop_browse();
	target_ = JoinTargetRecord{};
	state_ = JoinFlowState::Refused;
	if (!device_.show_refusal(text))
		cancel_choice();
}

void JoinFlow::join_from_novaworld(JoinTargetRecord target, uint32_t rid) {
	// The NovaWorld page's launch saves the player profile, then game.cfg, before it dials, as its
	// host leg does; a join that switches expansions saves the profile again at the switch
	// (docs/playerinfo/player-sav-re.md, docs/gamecfg/game-cfg-re.md).
	device_.save_profile();
	device_.save_config();
	joining_rid_ = rid;
	joining_novaworld_ = true;
	return_to_novaworld_ = true;
	join(std::move(target));
}

bool JoinFlow::launch(const LaunchFlags &flags) {
	launch_spectator_ = flags.spectator;
	launch_spectator_password_ = flags.spectator_password;
	if (const std::optional<LaunchHost> host = launch_host(flags)) {
		device_.start_launch_host(*host);
		return true;
	}
	const LanEndpoint endpoint = launch_lan_join_endpoint(flags, kRetailLanPortMin);
	if (endpoint.browse) {
		// `--lan-join browse[:<port>]` joins as a player does from the LAN screen: the search runs
		// on the LAN screen's defaults, and the join takes the first row on <port> (any row without
		// one), the endpoint rule a scripted retail joiner picks its row by.
		stop_browse();
		launch_port_ = launch_lan_join_endpoint(flags, 0).port;
		state_ = JoinFlowState::LaunchSearch;
		if (!device_.begin_browse(nullptr, serial_)) {
			io::logf(io::LogLevel::kWarn,
					"JoinFlow: --lan-join browse could not start the LAN search");
			state_ = JoinFlowState::Idle;
			stop_browse();
		}
		return true;
	}
	if (endpoint.ip.empty())
		return false;
	JoinTargetRecord target;
	target.host_ip = endpoint.ip;
	target.port = endpoint.port;
	apply_launch_role(target);
	join(std::move(target));
	return true;
}

// `--spectator` / `--spectator-password` answer the join's role question.
void JoinFlow::apply_launch_role(JoinTargetRecord &target) const {
	if (!launch_spectator_)
		return;
	target.join_role = JoinRole::Spectator;
	target.spectator_password = launch_spectator_password_;
	target.role_explicit = true;
}

// NovaWorld rows and direct targets do not carry ServerHello.P2: retail's enumerate-and-join
// browses the endpoint and joins only a session it discovered. The browse runs with no time limit,
// its first announce at once and then every 3 s, until a session answers or the player aborts
// (PRE_ENUMSESSION, ABORT_CANCEL); there is no row-less fallback dial (D-NET-703). It drains the
// process's enumerator first, the LAN screen's records each writing its removed line.
// [orig: UI_EnumerateAndJoinSession @0x569f50 - CNapiNetwork_DrainProtocolTimers @0x56a08c,
//  PRE_ENUMSESSION @0x56a0ac..0x56a0b4, the loop @0x56a0c1..0x56a0f9]
void JoinFlow::begin_enumerate() {
	++serial_;
	state_ = JoinFlowState::Enumerating;
	device_.drain_enumerator();
	if (!device_.begin_browse(&target_, serial_)) {
		io::logf(io::LogLevel::kWarn, "JoinFlow: the join could not enumerate %s:%d",
				target_.host_ip.c_str(), target_.port);
		abort_enumerate();
		return;
	}
	device_.show_enumerating();
}

void JoinFlow::browse_rows(uint32_t serial, const std::vector<DiscoveredSessionRow> &rows) {
	if (serial != serial_)
		return;
	if (state_ == JoinFlowState::Enumerating) {
		const int index = enumerated_row(rows);
		if (index >= 0)
			finish_enumerate(rows[static_cast<size_t>(index)]);
		return;
	}
	if (state_ != JoinFlowState::LaunchSearch)
		return;
	const int index = launch_search_row(rows, launch_port_);
	if (index < 0)
		return;
	JoinTargetRecord target = join_target_from_row(rows[static_cast<size_t>(index)]);
	// The join goes on with the search's socket (D-NET-1019) and its records, the process
	// enumerator's from here on; the search closes before the join opens anything of its own.
	device_.adopt_browse();
	state_ = JoinFlowState::Idle;
	stop_browse();
	apply_launch_role(target);
	join(std::move(target));
}

void JoinFlow::browse_finished(uint32_t serial) {
	if (serial != serial_ || state_ != JoinFlowState::LaunchSearch)
		return;
	io::logf(io::LogLevel::kWarn, "JoinFlow: --lan-join browse found no host on port %d",
			launch_port_);
	state_ = JoinFlowState::Idle;
	stop_browse();
}

void JoinFlow::finish_enumerate(const DiscoveredSessionRow &row) {
	apply_enumerated_row(target_, row);
	// The enumerate's socket goes onto the join and its records stay the process enumerator's
	// until the join's session reset drains them (ClientRuntime::drain_enumerator).
	device_.adopt_browse();
	state_ = JoinFlowState::Idle;
	stop_browse();
	// The discovered word reaches the choice when it carries any of 0x6038, else the join.
	// [orig: UI_EnumerateAndJoinSession @0x569f50 - `test g_PreGameServerFlags, 6038h` @0x56a2aa,
	//  state 8 @0x56a2cd, state 4 @0x56a2d9]
	if (join_entry_step(target_) == JoinEntryStep::Prompt)
		begin_choice();
	else
		dial();
}

// ABORT_CANCEL while the join enumerates: state 9. Its FullDestroy drains the enumerate's records,
// each writing its removed line (ClientRuntime::drain_enumerator carries the witness), then stops
// a held server and, off NovaWorld, tears the transport down with its line.
// [orig: PreMenu_OnAbortCancel @0x568CA0 (g_PreMenuAbortRequested @0x568caa, state 9);
//  MultiPlayer_JoinSessionStateMachine @0x56a320 - CNapiGameSession_FullDestroy(-1) called
//  @0x56a3e3, which keeps the transport under connect type 1 (CNapiGameSession_FullDestroy
//  @0x4C96A0, @0x4C9722)]
void JoinFlow::abort_enumerate() {
	const bool novaworld = target_.network_type == NetworkType::NovaWorld;
	device_.drain_browse();
	device_.destroy_pre_dial_session(novaworld);
	state_ = JoinFlowState::Idle;
	stop_browse();
	leave_abandoned_join();
}

// The join machine's choice (its state 8): pre.mnu's choice panels on the join screen (the walk
// and the routing are pre_game_menu.h's); ACCEPT's commit routes the three controls onto the
// target and the join goes on; BACK or Cancel leaves to the menu. Every join sends the automatic
// TR=-1 (D-NET-590).
void JoinFlow::begin_choice() {
	state_ = JoinFlowState::Choosing;
	if (!device_.show_choice(static_cast<uint32_t>(target_.server_flags))) {
		io::logf(io::LogLevel::kWarn, "JoinFlow: the join screen could not show the join choice");
		cancel_choice();
	}
}

void JoinFlow::commit_choice(
		const std::string &game_password, bool spectate, const std::string &team_password) {
	if (state_ != JoinFlowState::Choosing)
		return;
	state_ = JoinFlowState::Idle;
	apply_pre_game_choice(target_, game_password, spectate, team_password);
	dial();
}

// The choice's BACK or Cancel, or a refusal's Cancel: state 9 as above, its FullDestroy draining
// the process enumerator's records (the browse the join came from) before the transport goes.
void JoinFlow::cancel_choice() {
	// A refused LAN row has no target: the LAN screen's join.
	const bool novaworld =
			state_ == JoinFlowState::Choosing && target_.network_type == NetworkType::NovaWorld;
	state_ = JoinFlowState::Idle;
	device_.drain_enumerator();
	device_.destroy_pre_dial_session(novaworld);
	leave_abandoned_join();
}

void JoinFlow::cancel() {
	switch (state_) {
		case JoinFlowState::Enumerating:
			abort_enumerate();
			break;
		case JoinFlowState::Choosing:
		case JoinFlowState::Refused:
			cancel_choice();
			break;
		case JoinFlowState::LaunchSearch:
			state_ = JoinFlowState::Idle;
			stop_browse();
			break;
		case JoinFlowState::Idle:
			break;
	}
}

// State 9's tail past its FullDestroy: the game type zeroed, the main menu. A join entered from
// the NovaWorld menu leaves for the NovaWorld menu again with the session the destroy kept, as a
// normal exit from its match does: never stopped, so its socket stays open and writes no NET
// TEARING DOWN SOCKET line (inmatch/host_config.h write_transport_socket_teardown_line).
// [orig: MultiPlayer_JoinSessionStateMachine @0x56a320 - the main menu @0x56a3f9;
//  Menu_InitShellResources @0x552500 - UI_EnterNovaWorldMenu called @0x5526C2]
void JoinFlow::leave_abandoned_join() {
	joining_novaworld_ = false;
	device_.cancel_pre_game();
	device_.show_main_menu();
	device_.route_novaworld_session(take_return_to_novaworld());
}

// The dial: the callsign, then the switch to the host's expansion before the connect, the profile
// saved under the expansion it leaves and read again from the one it takes, so the character vars
// the join uploads and the spawn kit come from the host's data set; a failed switch leaves the
// post-auth reconcile on the session record to fail the join. Then the joiner load, which holds
// the join screen through the connect, the verification and the queue until the game starts
// (join_session_policy.h, the post-auth 0x7B authoritative over every browse-time hint).
// [orig: UI_JoinSelectedSession @0x5699d0 - the session's expansion @0x569afa,
//  Expansion_SwitchTo called @0x569b02; Expansion_SwitchTo @0x5688c0 - the pff test @0x568914,
//  PlayerProfile_SaveToFiles @0x56891a, PlayerProfile_LoadAllFromDisk @0x5689b4;
//  the join's session variables from the connect stream, Client_ParseServerSessionVariables
//  @0x5202f0; the screen held through NapiClient_SendPlayerSyncAndWaitForSyncTail @0x42cb20 then
//  NapiClient_WaitForGameStart @0x42cc10]
void JoinFlow::dial() {
	state_ = JoinFlowState::Idle;
	if (target_.player_name.empty())
		target_.player_name = device_.callsign();
	if (target_.expansion_known) {
		const std::string error = device_.switch_expansion(target_.expansion);
		if (error.empty()) {
			device_.save_profile();
			device_.reload_profile();
		} else {
			io::logf(io::LogLevel::kWarn, "JoinFlow: pre-dial expansion switch: %s", error.c_str());
		}
	}
	device_.start_join(target_);
}

std::optional<uint32_t> JoinFlow::join_failed(const ConnectionErrorRecord *record) {
	if (!joining_novaworld_)
		return std::nullopt;
	joining_novaworld_ = false;
	if (!refuses_this_install(record))
		return std::nullopt;
	return joining_rid_;
}

bool JoinFlow::take_return_to_novaworld() {
	const bool reenter = return_to_novaworld_;
	return_to_novaworld_ = false;
	return reenter;
}

} // namespace opennova::inmatch
