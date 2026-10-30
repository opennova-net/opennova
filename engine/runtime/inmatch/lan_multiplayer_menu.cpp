#include <runtime/inmatch/lan_multiplayer_menu.h>

#include <base/io/strutil.h>
#include <runtime/inmatch/game_config_block.h>
#include <runtime/inmatch/host_settings.h>
#include <runtime/menu/menu_runtime.h>

#include <utility>

namespace opennova::inmatch {

namespace {

using Kind = menu::MenuEvent::Kind;

constexpr const char *kLanScreen = "LAN_MULTI_PLAYER";
constexpr const char *kHostScreen = "MULTI_PLAYER_HOST";

// The host screen's controls the LAN screen's activation hides: the location and the LAN-only
// switch, each with its label (the LAN host advertises neither; the NovaWorld host, which never
// passes the LAN screen, shows them) [orig: UI_InitLANMultiplayerScreen @0x556960 -
// STATIC_GAME_LOCATION @0x5569f8, GAME_LOCATION @0x556a1a, STATIC_SERVER_LANONLY @0x556a3c,
// SERVER_LANONLY @0x556a5e].
constexpr const char *kLanHiddenHostControls[] = {
	"STATIC_GAME_LOCATION",
	"GAME_LOCATION",
	"STATIC_SERVER_LANONLY",
	"SERVER_LANONLY",
};

bool same_endpoint(const LanMenuServer &a, const LanMenuServer &b) {
	return a.host_ip == b.host_ip && a.port == b.port;
}

} // namespace

// The document opened (its INIT): the LAN browser's list shown, the host screen populated from
// the cfg block, its mission pool seeded and its START gated, then the opened screen's activation.
// Every owned screen's controls are addressable at once, so the open wires both screens whichever
// shows.
void LanMultiplayerMenu::open(menu::MenuRuntime &menu, const std::string &file,
		const std::string &screen) {
	(void)file;
	menu_ = &menu;
	if (menu.widget_id("LAN_GAME_LIST") >= 0) refresh_list_();
	// The host screen opens showing the process's cfg block (the populate the retail dialog's init
	// runs before its mission pool): the cap clamped to the screen's, each control from the block,
	// PunkBuster 0 and disabled (host_settings.h populate_host_screen carries the witness).
	if (host_) {
		if (GameConfigBlock *block = host_->config_block())
			if (block->title()) populate_host_screen(menu, block->cfg(), *block->title());
		if (menu.widget_id("MISSION_LIST") >= 0) dialog_.seed(menu, host_->host_pool());
	}
	sync_start_();
	activate_screen_(screen);
}

void LanMultiplayerMenu::close(menu::MenuRuntime &menu) {
	(void)menu;
	// The host screen is gone with the document: a NovaWorld host's setup ends with it.
	if (novaworld_) {
		novaworld_ = false;
		if (host_) host_->novaworld_host_cancelled();
	}
	menu_ = nullptr;
}

void LanMultiplayerMenu::seed_host_pool(const std::vector<menu::MissionChoice> &rows) {
	if (!menu_) return;
	dialog_.seed(*menu_, rows);
	sync_start_();
}

// A screen's activation. Leaving the host screen ends a NovaWorld host's setup. The LAN screen's
// empties its game list and drains the enumerator's session records, each writing its removed line
// to the process's _connectlog.txt (net/npwire/lan_discovery.h drain_records, D-NET-956), and
// hides the host screen's location and LAN-only controls [orig: UI_InitLANMultiplayerScreen
// @0x556960 - CListWnd_RemoveRow(-1) @0x5569bd, CNapiNetwork_DrainProtocolTimers @0x5569c7, both
// under the list's find @0x5569b7].
void LanMultiplayerMenu::activate_screen_(const std::string &screen) {
	if (novaworld_ && !strutil::iequals(screen, kHostScreen)) {
		novaworld_ = false;
		if (host_) host_->novaworld_host_cancelled();
	}
	if (!menu_ || !strutil::iequals(screen, kLanScreen)) return;
	if (menu_->widget_id("LAN_GAME_LIST") >= 0) {
		servers_.clear();
		selected_ = -1;
		error_.clear();
		refresh_list_();
		if (host_) host_->drain_records();
	}
	for (const char *name : kLanHiddenHostControls) {
		const int id = menu_->widget_id(name);
		if (id >= 0) menu_->set_widget_shown(id, false);
	}
}

// The list's rows: each listed session's row text (lan_discovery_row_label; the map is absent:
// the LAN enumeration has not joined the session, its identity arrives in the post-auth 0x7B
// stream). A failed search with nothing listed shows its reason in the list itself, an inert row
// (the join reads the servers, which stay empty).
void LanMultiplayerMenu::refresh_list_() {
	if (!menu_) return;
	const int id = menu_->widget_id("LAN_GAME_LIST");
	if (id < 0) return;
	std::vector<std::string> rows;
	rows.reserve(servers_.size());
	for (const LanMenuServer &server : servers_) rows.push_back(server.label);
	if (rows.empty() && !error_.empty()) rows.push_back("Search failed - " + error_);
	menu_->set_widget_items(id, rows);
}

// While the search's window runs LAN_SEARCH is non-interactive and reads MP_STATUS_SEARCHING; the
// window's end restores it with MP_SEARCH [orig: UI_LANMultiPlayer_OnSearch @0x558c50 -
// UIWidget_SetInteractiveRecursive(0) @0x558c7a, MP_STATUS_SEARCHING @0x558c94;
// UI_ProcessLANSessionStateMachine @0x558de0, the window's 0x7530 gate @0x55933a].
void LanMultiplayerMenu::set_search_state_(bool searching) {
	if (!menu_) return;
	const int id = menu_->widget_id("LAN_SEARCH");
	if (id < 0) return;
	menu_->set_widget_disabled(id, searching);
	const std::string text = host_ ? host_->menu_text(searching ? "MP_STATUS_SEARCHING" : "MP_SEARCH")
	                               : std::string();
	if (!text.empty()) menu_->set_widget_text(id, text);
}

// LAN_SEARCH: the browse begins (UI_StartLANSessionSearch @0x558300, the device's). A search that
// does not start keeps the button live and shows why in the list (OpenNova's: retail's search has
// no failure the player sees).
void LanMultiplayerMenu::search_() {
	error_.clear();
	if (host_ && host_->has_session()) {
		if (!host_->start_search()) {
			// The device reports the reason first (browse_error); a silent refusal says this.
			if (error_.empty()) browse_error("could not start the search");
			return;
		}
		set_search_state_(true);
	}
	refresh_list_();
}

void LanMultiplayerMenu::browse_finished() {
	set_search_state_(false);
}

void LanMultiplayerMenu::browse_error(const std::string &message) {
	error_ = message;
	refresh_list_();
}

// Within a browse window a newly discovered host is appended to the list and the player's
// highlighted row stays selected; a new search (which empties the list) or a snapshot that no
// longer holds the selected endpoint at its row drops the selection [orig:
// UI_ProcessLANSessionStateMachine @0x558de0 - the rows appended by UIList_AddRow @0x559516;
// UI_StartLANSessionSearch @0x558300 - the row table cleared @0x5583c1].
void LanMultiplayerMenu::servers_changed(std::vector<LanMenuServer> servers) {
	const bool keep = selected_ >= 0 && selected_ < static_cast<int>(servers_.size()) &&
	                  selected_ < static_cast<int>(servers.size()) &&
	                  same_endpoint(servers_[static_cast<size_t>(selected_)],
	                          servers[static_cast<size_t>(selected_)]);
	servers_ = std::move(servers);
	if (!keep) selected_ = -1;
	if (!servers_.empty()) error_.clear();
	refresh_list_();
	if (keep && menu_) {
		const int id = menu_->widget_id("LAN_GAME_LIST");
		if (id >= 0) menu_->select_row(id, selected_, false);
	}
}

// The join from the selected row (the list's double click and LAN_JOINGAME's press take one
// path): the press hands the frame to the join machine, so the LAN screen's browse pumps no more
// (its records and socket stay: LanDiscoveryBrowser::stop), then the join takes the listed
// session at the row's index, refused before anything is sent when the index names none, with
// gameerr's CVUNABLETOCREATE [orig: UI_LANMultiPlayer_OnJoinGame @0x556ce0 - its jump @0x556cea
// into UI_HandleLANGameListEvent @0x556c70 - the selected row walk @0x556bf4..0x556c0a,
// UI_SetSelectedSessionIndex @0x556c21, the PreMenu push @0x556c4e; UI_JoinSelectedSession
// @0x5699d0 - the node walk @0x569a4b..0x569a76, CVUNABLETOCREATE @0x569a78].
void LanMultiplayerMenu::join_() {
	if (!host_ || selected_ < 0 || selected_ >= static_cast<int>(servers_.size())) return;
	host_->stop_enumerator();
	if (!host_->join(selected_)) host_->join_refused(host_->error_text("MP Errors", "CVUNABLETOCREATE"));
}

void LanMultiplayerMenu::sync_start_() {
	if (!menu_) return;
	const int start = menu_->widget_id("START_GAME");
	if (start >= 0) menu_->set_widget_disabled(start, !dialog_.can_start());
}

// START: HOSTMNX hidden, then the screen read into the process's cfg block (HostDialog_ReadSettings,
// host_settings.h read_host_screen); the NovaWorld arm hosts through the NovaWorld panel, the LAN
// arm (on the screen holding MISSION_LIST) saves the block to game.cfg first; the session request
// is made of the block (host_config.h), the rotation and the expansion running (retail's game name
// field: the expansion the game mounted, empty for the base game) [orig: UI_HandleHostSessionStart
// @0x556d00 - HOSTMNX @0x556d50, the rotation walk @0x556dae, HostDialog_ReadSettings @0x556e36,
// the NovaWorld arm (networkConnectType 1) @0x556e47, the LAN arm (type 2 with MISSION_LIST)
// @0x556e65, Game_SaveConfig @0x556e72]. With no mission in the rotation START does nothing (its
// interactive gate: HostDialog::sync_start).
void LanMultiplayerMenu::host_start_() {
	if (!host_ || !menu_ || !dialog_.can_start()) return;
	const int mnx = menu_->widget_id("HOSTMNX");
	if (mnx >= 0) menu_->set_widget_shown(mnx, false);
	if (GameConfigBlock *block = host_->config_block())
		if (block->title()) read_host_screen(*menu_, block->cfg(), *block->title());
	LanHostRequest request;
	request.missions = dialog_.selected_missions();
	request.launch_options = dialog_.selected_launch_options();
	if (!request.missions.empty()) request.mission = request.missions.front();
	// The GAME_TYPE spin's selected row's value= (the filter's category; the selected mission's
	// mode owns the wire game type).
	const int spin = menu_->widget_id("GAME_TYPE");
	if (spin >= 0) request.game_type_attr = menu_->item_value(spin, menu_->selected_row(spin));
	request.expansion = host_->expansion();
	request.novaworld = novaworld_;
	if (novaworld_) {
		host_->host_start(request);
		return;
	}
	if (menu_->widget_id("MISSION_LIST") < 0) return;
	host_->save_config();
	host_->host_start(request);
}

void LanMultiplayerMenu::on_event(menu::MenuRuntime &menu, const menu::MenuEvent &event) {
	menu_ = &menu;
	switch (event.kind) {
	case Kind::ScreenChanged:
		activate_screen_(event.text);
		return;
	case Kind::WidgetActivated:
		if (strutil::iequals(event.text, "LAN_SEARCH")) search_();
		else if (strutil::iequals(event.text, "LAN_JOINGAME")) join_();
		else if (strutil::iequals(event.text, "ADD_MISSIONS"))
			dialog_.add_selected(menu, host_ ? host_->gametext() : hud::GameTextLookup());
		else if (strutil::iequals(event.text, "REMOVE_MISSIONS")) dialog_.remove_selected(menu);
		else if (strutil::iequals(event.text, "START_GAME")) host_start_();
		return;
	case Kind::ValueChanged:
		// A single click on the LAN list makes LAN_JOINGAME interactive; the double click joins
		// [orig: UI_HandleLANGameListEvent @0x556c70 - the 0x5000001 arm @0x556c79..0x556cb1].
		if (event.text2 == "list" && strutil::iequals(event.text, "LAN_GAME_LIST")) {
			selected_ = event.value;
			const int join = menu.widget_id("LAN_JOINGAME");
			if (join >= 0) menu.set_widget_disabled(join, false);
		} else if (event.text2 == "spinlist" && strutil::iequals(event.text, "GAME_TYPE")) {
			// [orig: HostDialog_FilterMissionListByGameType @0x556fe0]
			dialog_.filter(menu);
		}
		return;
	case Kind::ListActivated:
		// [orig: UI_HandleLANGameListEvent @0x556c70 - the 0x5000002 arm @0x556cbc]
		if (strutil::iequals(menu.widget_name_of(event.id), "LAN_GAME_LIST")) {
			selected_ = event.value;
			join_();
		}
		return;
	case Kind::TableCellClicked:
		// The SELECTED_MISSIONS Switch cell (column 2): a click toggles the row's launch option
		// (HostDialog::toggle_switch carries the witness).
		if (strutil::iequals(event.text, "SELECTED_MISSIONS") && event.column == 2)
			dialog_.toggle_switch(menu, event.value);
		return;
	default:
		return;
	}
}

} // namespace opennova::inmatch
