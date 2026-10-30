#pragma once

// The LAN multiplayer screens (mp.mnu's LAN_MULTI_PLAYER browser and the title's host settings
// screen, Title::host_screen: Jointops.exe's MULTI_PLAYER_HOST, dfx2.exe's NW_MULTI_PLAYER): what
// the game's code registers on their controls by name, the delegate the menu shell hands a
// document holding LAN_GAME_LIST or SELECTED_MISSIONS (menu::MenuCommands owner "lan")
// [orig: UI_RegisterLANMultiplayerCallbacks @0x558d20 - LAN_SEARCH
// UI_LANMultiPlayer_OnSearch @0x558c50, LAN_GAME_LIST UI_HandleLANGameListEvent @0x556c70,
// LAN_JOINGAME UI_LANMultiPlayer_OnJoinGame @0x556ce0; the host screen's START_GAME
// UI_HandleHostSessionStart @0x556d00]. The screens' models are the engine's: the host dialog's
// mission pool and rotation (menu::HostDialog), the cfg block the host screen shows and reads
// back (host_settings.h populate_host_screen / read_host_screen), its weapon and class
// restriction lists and their radios (host_settings.h populate_host_weapon_list /
// populate_host_class_list / host_restriction_list_pressed / host_restriction_radio_pressed)
// and the LAN list's row text
// (net/npwire/lan_discovery.h lan_discovery_row_label). The browse's socket, the join's dial and
// the session request are the host's (LanMultiplayerHost). Witness record:
// docs/net/novaworld-net-re.md, docs/mnu/menu-re.md.

#include <base/title/title.h>
#include <runtime/hud/game_text_lookup.h>
#include <runtime/menu/menu_flow.h>
#include <runtime/menu/menu_screen_controller.h>

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::inmatch {

class GameConfigBlock;

// One row of the LAN list: the session's address and the row's text (lan_discovery_row_label).
struct LanMenuServer {
	std::string host_ip;
	int port = 0;
	std::string label;
};

// What the host screen's START asks the game to host: the rotation's missions in table order and
// each row's launch option (its Switch cell), the rotation's head, the GAME_TYPE spin's selected
// value (the filter's category, not the wire game type: the selected mission's mode is), and the
// expansion the game runs ("" the base game), retail's game name field. The session's other
// words are the cfg block's, which the START read the screen into first.
struct LanHostRequest {
	std::vector<std::string> missions;
	std::vector<int32_t> launch_options;
	std::string mission;
	std::string game_type_attr;
	std::string expansion;
	bool novaworld = false;
};

class LanMultiplayerHost {
public:
	virtual ~LanMultiplayerHost() = default;
	// A menu token (menutxt's "Menu", then gameui's); "" when neither table holds it.
	virtual std::string menu_text(const std::string &key) = 0;
	// A gameerr string, its miss marker when the table holds none.
	virtual std::string error_text(const std::string &section, const std::string &key) = 0;
	// The gametext table (the rotation's GateTypeAbbrev cells, the weapon list's WepDes names) and
	// the menu's own resource, Game.bin (the restriction lists' tokens and class names).
	virtual hud::GameTextLookup gametext() = 0;
	virtual hud::GameTextLookup menu_ui() = 0;
	// The title a session over the menu's root runs as, the host pool's (HostDialog::seed: its
	// combat-vehicle filter and ADD Switch default).
	virtual const Title &session_title() = 0;
	// The process's cfg block the host screen shows and its START writes (null: none).
	virtual GameConfigBlock *config_block() = 0;
	// The mission catalog's rows the host pool is made of.
	virtual std::vector<menu::MissionChoice> host_pool() = 0;
	// The expansion the game runs ("" the base game).
	virtual std::string expansion() = 0;
	// The LAN session device: whether there is one, the search's start (false: it did not start;
	// the device reports why through browse_error), the enumerator stopped, its records drained.
	virtual bool has_session() = 0;
	virtual bool start_search() = 0;
	virtual void stop_enumerator() = 0;
	virtual void drain_records() = 0;
	// The join from the list's row `row`: the listed session at that row's index
	// (LanDiscoveryBrowser::join_session), or with no session device the row as listed. False
	// when it names none: the join is refused before anything is sent.
	virtual bool join(int row) = 0;
	virtual void join_refused(const std::string &text) = 0;
	// The cfg block written to game.cfg (the LAN START's save point).
	virtual void save_config() = 0;
	virtual void host_start(const LanHostRequest &request) = 0;
	// The NovaWorld host's setup left (the host screen gone): back to the LAN channel.
	virtual void novaworld_host_cancelled() = 0;
};

class LanMultiplayerMenu : public menu::MenuScreenController {
public:
	explicit LanMultiplayerMenu(LanMultiplayerHost *host = nullptr) : host_(host) {}
	void set_host(LanMultiplayerHost *host) { host_ = host; }

	const char *owner() const override { return "lan"; }
	void open(menu::MenuRuntime &menu, const std::string &file, const std::string &screen) override;
	void on_event(menu::MenuRuntime &menu, const menu::MenuEvent &event) override;
	void close(menu::MenuRuntime &menu) override;
	// Whether it holds a document now.
	bool is_open() const { return menu_ != nullptr; }

	// --- the LAN session device's reports ---
	// The browse's rows as they stand: the list refilled; the highlighted row stays selected while
	// the new rows hold its session at its row (a browse appends), else the selection goes.
	void servers_changed(std::vector<LanMenuServer> servers);
	// The search failed: the reason shows in the empty list.
	void browse_error(const std::string &message);
	// The browse window closed: LAN_SEARCH is interactive again with its MP_SEARCH text.
	void browse_finished();

	// The host pool made anew from `rows` (the open seeds it from the host's catalog).
	void seed_host_pool(const std::vector<menu::MissionChoice> &rows);
	// The host screen hosts on NovaWorld (the NovaWorld panel's host setup) instead of the LAN.
	void set_novaworld_host(bool novaworld) { novaworld_ = novaworld; }
	bool novaworld_host() const { return novaworld_; }

	// --- readers ---
	const std::vector<LanMenuServer> &servers() const { return servers_; }
	int selected_server() const { return selected_; }
	const std::string &browse_error_text() const { return error_; }
	const menu::HostDialog &host_dialog() const { return dialog_; }

private:
	void activate_screen_(const std::string &screen);
	void populate_restrictions_();
	bool restriction_list_pressed_(const menu::MenuEvent &event);
	bool restriction_radio_pressed_(const std::string &control);
	void refresh_list_();
	void set_search_state_(bool searching);
	void search_();
	void join_();
	void host_start_();
	void sync_start_();

	LanMultiplayerHost *host_ = nullptr;
	menu::MenuRuntime *menu_ = nullptr; // the runtime of the document it holds (null: none)
	menu::HostDialog dialog_;
	std::vector<LanMenuServer> servers_;
	int selected_ = -1;
	std::string error_;
	bool novaworld_ = false;
};

} // namespace opennova::inmatch
