#pragma once

// pre.mnu's PRE_GAME_MENU screen, the join screen, as the PreMenu mode registers it: ACCEPT to the
// panel machine's walk, BACK and ABORT_CANCEL to the abort, the screen opened on the progress panel
// [orig: PreMenu_Init @0x5693E0 — the load @0x56944a, the select @0x56945c, ACCEPT ->
// UI_PreGameMenuStateMachine @0x56947a, BACK -> PreMenu_OnAbortCancel @0x569498, ABORT_CANCEL ->
// PreMenu_OnAbortCancel @0x5694b6, g_PreGamePanelState = 0 @0x5694bb]. The panel table, the walk
// and the status lines are pre_game_menu.h's; this controller holds the screen's panel, the
// discovered flag word the choice walks and the join it follows, and writes MESSAGES. The embedder
// reports the abort and the commit outward and hands the join's status each frame.

#include <cstdint>
#include <string>

#include <runtime/inmatch/pre_game_menu.h>
#include <runtime/menu/menu_screen_controller.h>

namespace opennova::inmatch {

class PreGameController : public menu::MenuScreenController {
public:
	class Host {
	public:
		virtual ~Host() = default;
		// BACK, ABORT_CANCEL or ACCEPT's abort: the join is abandoned.
		virtual void cancelled() = 0;
		// The choice's last ACCEPT: the three controls as the commit reads them (pre_game_commit).
		virtual void committed(const std::string &game_password, bool spectate,
				const std::string &team_password) = 0;
	};
	void set_host(Host *host) { host_ = host; }

	// The screen opened: the progress panel with an empty status line, following no join.
	void open(menu::MenuRuntime &menu, const std::string &file, const std::string &screen) override;
	void on_event(menu::MenuRuntime &menu, const menu::MenuEvent &event) override;
	void close(menu::MenuRuntime &menu) override;

	// The join's progress: the status line with Cancel.
	void show_progress(menu::MenuRuntime &menu, const std::string &text);
	// A failed join: the reason with Cancel only, following no join.
	void show_failure(menu::MenuRuntime &menu, const std::string &text);
	// The join machine's choice: the first panel the flag word asks for (pre_game_entry_panel);
	// false when the word offers none.
	bool show_choice(menu::MenuRuntime &menu, uint32_t server_flags);
	// ACCEPT on the showing panel (pre_game_accept): the next panel, the commit (the screen back on
	// its progress panel, the three controls reported) or the abort.
	void accept(menu::MenuRuntime &menu);

	// The join the screen follows: each frame on the progress panel writes its status line (with
	// `has_status`), and the game start's line, once shown a frame, hands over to the loading
	// screen.
	void follow(bool following) {
		following_ = following;
		starting_shown_ = false;
	}
	bool following() const { return following_; }
	enum class FollowStep { Idle, Shown, HandOff };
	FollowStep follow_frame(menu::MenuRuntime &menu, bool has_status, JoinScreenStage stage,
			const std::string &text);

	PreGamePanel panel() const { return panel_; }

private:
	void set_panel_(menu::MenuRuntime &menu, PreGamePanel panel);
	void set_message_(menu::MenuRuntime &menu, const std::string &text);

	Host *host_ = nullptr;
	PreGamePanel panel_ = PreGamePanel::Progress;
	uint32_t choice_flags_ = 0;
	bool following_ = false;
	bool starting_shown_ = false;
};

} // namespace opennova::inmatch
