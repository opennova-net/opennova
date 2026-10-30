#pragma once

// The DEATH screen (death.mnu's DEATH): the deploy map a mission opens for the authority's initial
// deployment hold and for a dead player's pick, its controls the code registers by name and its
// per-frame reveal and refresh, over the deploy feed (world/deploy_screen_feed.h, where the row
// loops, the statics' texts and the team-service gate carry their witnesses).
// [orig: death.mnu <NAME>DEATH</NAME>; UI_RegisterDeathScreenCallbacks @0x554610 (CONFIRM_YES
//  DeathScreen_OnConfirmYes @0x553530 @0x554647, CONFIRM_NO DeathScreen_OnConfirmNo @0x553680
//  @0x554665, SWAP_TEAMS DeathScreen_OnSwapTeams @0x5535b0 @0x554683, SPAWNPOINTS_LIST
//  DeathScreen_OnSpawnListSelect @0x553630 @0x5546a1, HIDDEN_BACK DeathScreen_OnHiddenBack
//  @0x553660 @0x5546c4); DeathScreen_UpdateUI @0x553150 (the show: CWnd_SetShown(DEATH_SHROUD, 0)
//  @0x55325f, SWAP_TEAMS / BUTTON_TEAMLIST @0x553489 / @0x5534AA); DeathScreen_UpdateShroudReveal
//  @0x554730 (the reveal @0x554771 and the refresh on (tick & 0xF) == 0 @0x55477d, run from
//  UI_ProcessMenuScreenFrame @0x54e67c); UI_UpdateDeathScreenContent @0x5536a0 (the list and the
//  statics)]
//
// The screen's lifetime is the frame loop's: it stays up while a trigger holds or a join pick is
// owed (the embedder's held()), and its close is the scene frame's, so a frame a listen host's
// status page stood in for closes nothing (the witnesses live on the client state's
// death_menu_open_latch and hud-re D-HUD-19). A dead session tears it down instead of closing it:
// retail's disconnect exits the mission outright on that edge, it never resumes play
// [orig: CNapiNetwork_OnDisconnectedFromServer @0x4c63d0 -> Input_QueueEvent(3) @0x4c67a4 ->
//  g_MissionExitReason = 1 (Input_HandleActionBinding case 3 @0x49af2c), the nav-push "MainMenu"
//  teardown every abort leg takes @0x568654]. Witness record: docs/interface/hud-re.md (D-HUD-19).

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <runtime/hud/game_text_lookup.h>
#include <runtime/menu/menu_screen_controller.h>
#include <runtime/world/deploy_screen_feed.h>

namespace opennova::menu {

// What the DEATH screen reads of the session and sends to it: the embedder's role view (a joiner's
// replica state or the authority's own facts).
class DeathScreenHost {
public:
	virtual ~DeathScreenHost() = default;
	// The screen stays up: a trigger holds (the deploy overlay, an in-session death) or a join pick
	// is owed.
	virtual bool held() = 0;
	// The join owes the player a pick or awaits the host's release of one; the local player's dead
	// bit.
	virtual bool pick_pending() = 0;
	virtual bool local_player_dead() = 0;
	// The session died under the screen (the host's punt).
	virtual bool session_lost() = 0;
	// The shroud's reveal test (world::death_shroud_revealed over the role's facts) and the logic tick.
	virtual bool shroud_revealed() = 0;
	virtual int64_t logic_tick() = 0;
	// The statics' facts for this client (the timers, the queued line, the gates, their texts
	// resolved through the gametext table with the MedicReq binding's label).
	virtual world::DeployScreenStatus status() = 0;
	// The compiled SPAWNPOINTS_LIST rows (world::build_deploy_rows over the role's zone rows and
	// team colour): the Default row's key and home, and a zone name key's text.
	virtual std::vector<world::DeployListRow> list_rows(const std::string &default_key,
			const std::string &default_home,
			const std::function<std::string(const std::string &key)> &zone_name) = 0;
	// The pick's send (Input_HandleActionBinding case 12): true when it was sent.
	virtual bool send_pick(int param) = 0;
	// SWAP_TEAMS: the other side asked for (C2S 0x4D).
	virtual void send_team_change() = 0;
	// The screen closes now (a pick under the initial overlay, the exit's YES).
	virtual void close_screen() = 0;
	// CONFIRM_YES, after the close: the shell runs input action 3 (the Exit Mission tone, then the
	// exit; engine world::pass_through_close_axis carries the key witness).
	virtual void exit_confirmed() = 0;
};

class DeathScreen final : public MenuScreenController {
public:
	static constexpr const char *kFile = "death.mnu";
	static constexpr const char *kScreen = "DEATH";

	// One visible SPAWNPOINTS_LIST row: its label as the list holds it (the tags stripped, the page's
	// bytes) and the node parameter its pick sends (0 the Default Spawn, index + 1 a zone, -1 an
	// occupant or the blank separator: never a pick).
	struct SpawnRow {
		std::string label;
		int param = 0;
	};
	// What one frame of the open screen leaves.
	enum class Frame {
		Keep,
		Close, // its triggers fell on a drawn scene frame
		Teardown, // the session died under it
	};

	void set_host(DeathScreenHost *host) { host_ = host; }
	// The tables its text reads: the gametext (WPNames), menutxt and Game.bin (their Menu sections).
	void set_text(hud::GameTextLookup gametext, hud::GameTextLookup menutxt, hud::GameTextLookup gameui);

	// Whether the screen opens: a trigger holds or a pick is owed.
	bool can_open() const;
	// One main frame while the screen is up: the teardown on a dead session, the close once its
	// triggers fell on a drawn scene frame, else the shroud's reveal and, inside it, the content
	// refresh on each 16-tick boundary.
	Frame frame(MenuRuntime &menu, bool scene_drawn);
	// A pick of `param` (a list row's, or a deploy key's with the screen open or not): sent; true
	// when the screen closes on it (under the initial overlay; a death re-pick keeps the screen
	// until the host releases the hold, which may drop an invalid or contested pick silently).
	bool pick(int param);
	// The menu input pass-through word while DEATH is up: its show raises it, the exit
	// confirmation (HIDDEN_BACK) lowers it and the confirmation's NO raises it again; the close
	// leaves it as it stands.
	bool pass_through() const { return pass_through_; }
	const std::vector<SpawnRow> &rows() const { return rows_; }
	// The screen torn down (its menu rebuilt next time): the row model goes.
	void reset();

	// MenuScreenController: the show (the team-service pair, the shroud hidden, the word raised,
	// the tick stamped); the registered controls' events.
	void open(MenuRuntime &menu, const std::string &file, const std::string &screen) override;
	void on_event(MenuRuntime &menu, const MenuEvent &event) override;

private:
	// The under-the-initial-overlay test: no pick owed and the local player alive.
	bool initial_overlay_() const;
	void refresh_(MenuRuntime &menu);
	void apply_statics_(MenuRuntime &menu, const world::DeployScreenStatus &status);
	void apply_team_buttons_(MenuRuntime &menu, bool shown);
	void set_shroud_shown_(MenuRuntime &menu, bool shown);
	std::string menu_text_(const char *key, const char *fallback) const;

	DeathScreenHost *host_ = nullptr;
	hud::GameTextLookup gametext_, menutxt_, gameui_;
	std::vector<SpawnRow> rows_;
	int64_t last_tick_ = 0;
	bool pass_through_ = false;
};

} // namespace opennova::menu
