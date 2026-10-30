// The PreMenu join screen (pre.mnu PRE_GAME_MENU): the windows each panel state
// shows, the host's S2C 0x03 queue record as the client folds it, and the
// status line each join stage writes into MESSAGES; and the screen's controller
// (inmatch/pre_game_controller.h) over an authored pre.mnu through the menu runtime:
// the open on the progress panel, the status line replacing MESSAGES, the failure's
// panel, the choice walked by ACCEPT to the commit of the three controls, BACK and
// ABORT_CANCEL abandoning the join, and the followed join's one-frame handoff.
// [orig: UI_ShowPreGameMenuByState @0x568d10; NapiNPClientMsg_0x003 @0x425390;
//  MultiPlayer_JoinSessionStateMachine @0x56a320; PreMenu_Init @0x5693E0]

#include <formats/mnu/mnu.h>
#include <runtime/inmatch/pre_game_controller.h>
#include <runtime/inmatch/pre_game_menu.h>
#include <runtime/menu/menu_runtime.h>
#include <runtime/menu/menu_state_frame.h>

#include <cstdio>
#include <string>
#include <vector>
#include "../common/test_expect.h"

using namespace opennova;
using inmatch::JoinQueueRecord;
using inmatch::JoinScreenStage;
using inmatch::PreGamePanel;
using inmatch::PreGameAccept;
using inmatch::PreGameChoice;
using inmatch::pre_game_accept;
using inmatch::pre_game_commit;
using inmatch::pre_game_entry_panel;

namespace {

using test_expect::expect;

const rtxt::File &gameerr() {
	static const rtxt::File file = [] {
		rtxt::File f;
		f.sections.push_back(rtxt::Section{"Generic Strings", 0});
		const std::vector<std::pair<std::string, std::string>> rows = {
				{"PRE_ENUMSESSION", "(the PRE_ENUMSESSION row)"},
				{"PRE_JOININGSESSION", "Joining Session..."},
				{"PRE_CONNECTING", "Connecting to server..."},
				{"PRE_VERIFYING", "Verifying Session..."},
				{"PRE_CONNECTED", "Starting game..."},
				{"WAITTOJOIN", "Waiting in line to join server...."},
				{"WAITXXXPEOPLEINFRONT1", "Waiting to join.  Please wait, there are %ld people in front of you."},
				{"WAITXXXPEOPLEINFRONT2", "Waiting to join.  Please wait, there is %ld person in front of you."},
				{"WAITXXXPEOPLEINFRONT3", "Waiting to join.  Please wait, you are next in line."},
				{"WAITTIMEDISPLAY", "[%2.2ld:%2.2ld:%2.2ld]"},
		};
		for (const auto &row : rows) {
			rtxt::Entry entry;
			entry.key = row.first;
			entry.text = row.second;
			f.entries.push_back(entry);
			++f.sections[0].string_count;
		}
		return f;
	}();
	return file;
}

std::string text(JoinScreenStage stage, const JoinQueueRecord &queue, uint64_t now_ms) {
	return inmatch::join_screen_text(stage, queue, now_ms, nullptr, &gameerr());
}

bool run_each_panel_shows_its_windows() {
	using namespace inmatch;
	if (!expect(pre_game_windows(PreGamePanel::Progress) ==
					(kPreGameErrorWrapper | kPreGameAbortWrapper),
			"progress: the MESSAGES box and Cancel"))
		return false;
	if (!expect(pre_game_windows(PreGamePanel::Error) ==
					(kPreGameErrorWrapper | kPreGameAbortWrapper),
			"error: the reason in MESSAGES and Cancel only"))
		return false;
	if (!expect(pre_game_windows(PreGamePanel::GamePassword) ==
					(kPreGameGamePasswordWrapper | kPreGameAbortRetryWrapper),
			"game password: its panel with BACK / ACCEPT"))
		return false;
	if (!expect(pre_game_windows(PreGamePanel::SpectatorPassword) ==
					(kPreGameTeamPasswordWrapper | kPreGameSpectatorStaticWrapper |
							kPreGameAbortRetryWrapper),
			"spectator password: the team-password box under the spectator text"))
		return false;
	return expect(std::string(pre_game_window_name(0)) == "ERROR_WRAPPER" &&
					std::string(pre_game_window_name(7)) == "SPECTATOR_STATIC_WRAPPER",
			"the window names follow the bit order");
}

bool run_the_queue_record_folds_like_retail() {
	JoinQueueRecord queue;
	const uint8_t first[] = {0x01, 0x05, 0x00, 0x09, 0x00};
	inmatch::fold_join_queue_record(queue, first, sizeof(first), 1000);
	if (!expect(queue.queued && queue.position == 5 && queue.length == 9 &&
					queue.queued_since_ms == 1000,
			"a queued record stores position and length and stamps the start"))
		return false;
	const uint8_t moved[] = {0x01, 0x04, 0x00, 0x09, 0x00};
	inmatch::fold_join_queue_record(queue, moved, sizeof(moved), 5000);
	if (!expect(queue.position == 4 && queue.queued_since_ms == 1000,
			"a later queued record keeps the first stamp"))
		return false;
	const uint8_t out[] = {0x00, 0x04, 0x00, 0x09, 0x00};
	inmatch::fold_join_queue_record(queue, out, sizeof(out), 6000);
	if (!expect(!queue.queued && queue.position == 0 && queue.length == 0,
			"an unqueued record clears position and length"))
		return false;
	const uint8_t short_body[] = {0x01, 0x02};
	inmatch::fold_join_queue_record(queue, short_body, sizeof(short_body), 7000);
	return expect(queue.queued && queue.position == 0 && queue.length == 0 &&
					queue.queued_since_ms == 7000,
			"fields past the body read 0; a new queued run restamps");
}

bool run_each_stage_writes_its_line() {
	const JoinQueueRecord none;
	if (!expect(text(JoinScreenStage::Joining, none, 0) == "Joining Session..." &&
					text(JoinScreenStage::Connecting, none, 0) == "Connecting to server..." &&
					text(JoinScreenStage::Verifying, none, 0) == "Verifying Session..." &&
					text(JoinScreenStage::Starting, none, 0) == "Starting game...",
			"the dial, connect, verify and start lines"))
		return false;
	// The enumerate-and-join's browse reads PRE_ENUMSESSION (D-NET-703)
	// [orig: UI_EnumerateAndJoinSession @0x569f50 - @0x56a0ac..0x56a0b4].
	if (!expect(text(JoinScreenStage::Enumerating, none, 0) == "(the PRE_ENUMSESSION row)",
			"the enumerate line"))
		return false;
	if (!expect(text(JoinScreenStage::Queued, none, 0) == "Waiting in line to join server....",
			"a queue with no valid position reads WAITTOJOIN"))
		return false;
	JoinQueueRecord queue;
	queue.queued = true;
	queue.position = 5;
	queue.length = 9;
	queue.queued_since_ms = 10000;
	if (!expect(text(JoinScreenStage::Queued, queue, 10000 + 3725000) ==
					"Waiting to join.  Please wait, there are 4 people in front of you. [01:02:05]",
			"position 5: four ahead, then the time queued"))
		return false;
	queue.position = 2;
	if (!expect(text(JoinScreenStage::Queued, queue, 10000) ==
					"Waiting to join.  Please wait, there is 1 person in front of you. [00:00:00]",
			"position 2: one ahead"))
		return false;
	queue.position = 1;
	if (!expect(text(JoinScreenStage::Queued, queue, 10000 + 59999) ==
					"Waiting to join.  Please wait, you are next in line. [00:00:59]",
			"position 1: next in line"))
		return false;
	queue.position = 10;
	return expect(text(JoinScreenStage::Queued, queue, 10000) ==
					"Waiting in line to join server....",
			"a position past the queue length reads WAITTOJOIN");
}

} // namespace

// The choice panels (L2-s2 FOUND 1): the entry panel by the flag word, ACCEPT's walk and
// the commit's copy-out, each against the cited arms.
// [orig: UI_ShowPreGameMenuByState @0x568dfc..0x568e44; UI_PreGameMenuStateMachine
//  @0x568A40 (cases 1..4, the default's abort, the commit @0x568b7e..0x568c69)]
bool run_the_choice_panels_follow_the_flag_word() {
	// Entry: 0x8 first, then 0x2000, then 0x30 off an authority; else none.
	if (!expect(pre_game_entry_panel(0x8 | 0x2000 | 0x30, false) == PreGamePanel::GamePassword &&
	                    pre_game_entry_panel(0x2000 | 0x30, false) == PreGamePanel::Spectate &&
	                    pre_game_entry_panel(0x10, false) == PreGamePanel::TeamPassword &&
	                    pre_game_entry_panel(0x20, true) == PreGamePanel::Progress &&
	                    pre_game_entry_panel(0x4000, false) == PreGamePanel::Progress,
			"entry: the server password, then spectate, then a side password off an authority"))
		return false;
	const auto next = [](PreGamePanel panel, uint32_t flags, bool checked, bool authority) {
		return pre_game_accept(panel, flags, checked, authority);
	};
	PreGameAccept a = next(PreGamePanel::GamePassword, 0x8 | 0x2000 | 0x30, false, false);
	if (!expect(a.next == PreGamePanel::Spectate && !a.commit,
			"GAME_PASSWORD's ACCEPT goes on to SPECTATE when the word takes spectators"))
		return false;
	a = next(PreGamePanel::GamePassword, 0x8 | 0x30, false, false);
	if (!expect(a.next == PreGamePanel::TeamPassword && !a.commit,
			"else to TEAM_PASSWORD for a side password"))
		return false;
	a = next(PreGamePanel::GamePassword, 0x8 | 0x30, false, true);
	if (!expect(a.commit && a.next == PreGamePanel::Progress,
			"an authority owes no side password: GAME_PASSWORD commits"))
		return false;
	a = next(PreGamePanel::Spectate, 0x2000 | 0x4000 | 0x30, true, false);
	if (!expect(a.next == PreGamePanel::SpectatorPassword && !a.commit,
			"SPECTATE checked goes on to the spectator password"))
		return false;
	a = next(PreGamePanel::Spectate, 0x2000 | 0x30, true, false);
	if (!expect(a.commit, "checked with no spectator password: commits"))
		return false;
	a = next(PreGamePanel::Spectate, 0x2000 | 0x4000 | 0x30, false, false);
	if (!expect(a.next == PreGamePanel::TeamPassword && !a.commit,
			"unchecked goes on to TEAM_PASSWORD for a side password"))
		return false;
	a = next(PreGamePanel::Spectate, 0x2000 | 0x4000, false, false);
	if (!expect(a.commit, "unchecked with no side password: commits"))
		return false;
	if (!expect(next(PreGamePanel::SpectatorPassword, 0x6038, true, false).commit &&
	                    next(PreGamePanel::TeamPassword, 0x6038, false, false).commit,
			"either password panel commits"))
		return false;
	a = next(PreGamePanel::Progress, 0x6038, false, false);
	if (!expect(!a.commit && !a.abort && a.next == PreGamePanel::Progress,
			"ACCEPT on Progress does nothing"))
		return false;
	if (!expect(next(PreGamePanel::Error, 0x6038, false, false).abort,
			"ACCEPT on Error aborts the join"))
		return false;
	// The commit: the shared TEAM_PASSWORD text to JSPP when spectating, else to JSP;
	// the other keeps what it held.
	PreGameChoice choice;
	choice.side_password = "old-side";
	choice.spectator_password = "old-spec";
	pre_game_commit("srv", true, "shared", choice);
	if (!expect(choice.server_password == "srv" && choice.spectate &&
	                    choice.spectator_password == "shared" && choice.side_password == "old-side",
			"spectating: the text is the spectator password, the side password kept"))
		return false;
	pre_game_commit("", false, "team", choice);
	return expect(choice.server_password.empty() && !choice.spectate &&
	                      choice.side_password == "team" && choice.spectator_password == "shared",
			"playing: the text is the side password, the spectator password kept");
}

namespace {

// pre.mnu's PRE_GAME_MENU of the game's shape: the panels' wrappers and their controls.
std::string pre_window(const char *type, const char *name, int top, const std::string &inner,
		const char *attrs) {
	char position[160];
	std::snprintf(position, sizeof(position),
			"<POSITION><LEFT>20</LEFT><TOP>%d</TOP><RIGHT>260</RIGHT><BOTTOM>%d</BOTTOM></POSITION>", top,
			top + 20);
	return std::string("<WINDOW type=\"") + type + "\" name=\"" + name + "\"" + attrs + ">" + inner +
	       position + "</WINDOW>";
}

std::string pre_mnu() {
	const auto w = pre_window;
	const std::string body =
			w("window", "GAME_PASSWORD_WRAPPER", 20, w("edit", "GAME_PASSWORD", 30, "", ""), " HIDDEN") +
			w("window", "SPECTATE_WRAPPER", 60, w("checkbox", "SPECTATE", 70, "", ""), " HIDDEN") +
			w("window", "TEAM_PASSWORD_WRAPPER", 100,
					w("window", "TEAM_STATIC_WRAPPER", 110, "", " HIDDEN") +
							w("window", "SPECTATOR_STATIC_WRAPPER", 130, "", " HIDDEN") +
							w("edit", "TEAM_PASSWORD", 150, "", ""),
					" HIDDEN") +
			w("window", "ERROR_WRAPPER", 200, w("multiline_edit", "MESSAGES", 210, "", " READONLY"), " HIDDEN") +
			w("window", "ABORT_WRAPPER", 300, w("button", "ABORT_CANCEL", 310, "", ""), " HIDDEN") +
			w("window", "ABORTRETRY_WRAPPER", 400,
					w("button", "BACK", 410, "", "") + w("button", "ACCEPT", 440, "", ""), " HIDDEN");
	return "<SCREEN><NAME>PRE_GAME_MENU</NAME><WINDOW type=\"window\" name=\"MAIN\"><POSITION><LEFT>0</LEFT>"
	       "<TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>" +
	       body + "</WINDOW></SCREEN>";
}

struct JoinScreen : inmatch::PreGameController::Host {
	mnu::Document doc;
	menu::MenuStateFrame frame;
	menu::MenuRuntime rt;
	inmatch::PreGameController controller;
	int cancels = 0;
	std::vector<std::string> commits;

	void cancelled() override { ++cancels; }
	void committed(const std::string &game_password, bool spectate, const std::string &team_password) override {
		commits.push_back(game_password + "|" + (spectate ? "1" : "0") + "|" + team_password);
	}
	bool open() {
		std::string error;
		if (!mnu::parse(pre_mnu(), doc, error)) return false;
		frame.set_configure([this](const std::string &screen, menu::MenuFrameCompiler &compiler) {
			const mnu::Screen *found = screen.empty() ? doc.first_screen() : doc.find_screen(screen);
			compiler.configure(found);
			return found != nullptr;
		});
		rt.set_frame(&frame);
		// The shell's hand-off: the claimed controller hears its document's events.
		rt.set_sink([this](const menu::MenuEvent &e) { controller.on_event(rt, e); });
		controller.set_host(this);
		if (!rt.open_document(&doc, "pre.mnu", "PRE_GAME_MENU")) return false;
		controller.open(rt, "pre.mnu", rt.current_screen());
		return true;
	}
	bool shown(const char *name) const { return rt.is_widget_shown(rt.widget_id(name)); }
	std::string message() const { return rt.get_widget_text(rt.widget_id("MESSAGES")); }
	void press(const char *name) { rt.activate(rt.widget_id(name)); }
};

bool run_the_controller_runs_the_screen() {
	JoinScreen s;
	if (!expect(s.open(), "controller: pre.mnu opens")) return false;
	if (!expect(s.controller.panel() == PreGamePanel::Progress && s.shown("ERROR_WRAPPER") &&
	                    s.shown("ABORT_WRAPPER") && !s.shown("ABORTRETRY_WRAPPER") && s.message().empty(),
			"controller: the open is the progress panel, its line empty"))
		return false;
	s.controller.show_progress(s.rt, "Connecting to server...");
	s.controller.show_progress(s.rt, "Verifying Session...");
	if (!expect(s.message() == "Verifying Session...", "controller: the status line replaces the text"))
		return false;
	s.controller.show_failure(s.rt, "Your game is incompatible with this server. (NCC007)");
	if (!expect(s.controller.panel() == PreGamePanel::Error && s.shown("ABORT_WRAPPER") &&
	                    !s.shown("ABORTRETRY_WRAPPER") &&
	                    s.message() == "Your game is incompatible with this server. (NCC007)",
			"controller: a failure parks the reason with Cancel only"))
		return false;
	s.press("ABORT_CANCEL");
	if (!expect(s.cancels == 1, "controller: ABORT_CANCEL abandons the join")) return false;
	// The choice: a plain team game offers none; a server password, spectators and a spectator
	// password walk GAME_PASSWORD, SPECTATE (checked) and the spectator password to the commit.
	if (!expect(!s.controller.show_choice(s.rt, 0x1), "controller: a plain team game offers no choice"))
		return false;
	if (!expect(s.controller.show_choice(s.rt, 0x8 | 0x2000 | 0x4000) &&
	                    s.controller.panel() == PreGamePanel::GamePassword &&
	                    s.shown("GAME_PASSWORD_WRAPPER") && s.shown("ABORTRETRY_WRAPPER") &&
	                    !s.shown("ERROR_WRAPPER"),
			"controller: the server password opens GAME_PASSWORD"))
		return false;
	s.rt.set_widget_text(s.rt.widget_id("GAME_PASSWORD"), "srv");
	s.press("ACCEPT");
	if (!expect(s.controller.panel() == PreGamePanel::Spectate && s.shown("SPECTATE_WRAPPER"),
			"controller: ACCEPT goes on to SPECTATE"))
		return false;
	s.rt.set_widget_checked(s.rt.widget_id("SPECTATE"), true);
	s.press("ACCEPT");
	if (!expect(s.controller.panel() == PreGamePanel::SpectatorPassword &&
	                    s.shown("SPECTATOR_STATIC_WRAPPER") && !s.shown("TEAM_STATIC_WRAPPER"),
			"controller: SPECTATE checked goes on to the spectator password"))
		return false;
	s.rt.set_widget_text(s.rt.widget_id("TEAM_PASSWORD"), "spec");
	s.press("ACCEPT");
	if (!expect(s.commits.size() == 1 && s.commits[0] == "srv|1|spec" &&
	                    s.controller.panel() == PreGamePanel::Progress,
			"controller: the last ACCEPT commits the three controls, back on the progress panel"))
		return false;
	// A side password off a plain player: SPECTATE unchecked goes on to TEAM_PASSWORD; BACK abandons.
	if (!expect(s.controller.show_choice(s.rt, 0x2000 | 0x10), "controller: a side password's choice"))
		return false;
	s.rt.set_widget_checked(s.rt.widget_id("SPECTATE"), false);
	s.press("ACCEPT");
	if (!expect(s.controller.panel() == PreGamePanel::TeamPassword && s.shown("TEAM_STATIC_WRAPPER"),
			"controller: SPECTATE unchecked goes on to TEAM_PASSWORD"))
		return false;
	s.press("BACK");
	if (!expect(s.cancels == 2 && s.commits.size() == 1, "controller: BACK abandons the choice"))
		return false;
	// The followed join: its lines on the progress panel, the start's shown a frame, then the handoff.
	using Step = inmatch::PreGameController::FollowStep;
	s.controller.show_progress(s.rt, std::string());
	s.controller.follow(true);
	if (!expect(s.controller.follow_frame(s.rt, true, JoinScreenStage::Queued, "Waiting in line") ==
	                            Step::Shown &&
	                    s.message() == "Waiting in line",
			"follow: the join's line"))
		return false;
	if (!expect(s.controller.follow_frame(s.rt, false, JoinScreenStage::Queued, "") == Step::Idle &&
	                    s.message() == "Waiting in line",
			"follow: no status, no line"))
		return false;
	if (!expect(s.controller.follow_frame(s.rt, true, JoinScreenStage::Starting, "Starting game...") ==
	                            Step::Shown &&
	                    s.message() == "Starting game...",
			"follow: the start's line shows"))
		return false;
	if (!expect(s.controller.follow_frame(s.rt, true, JoinScreenStage::Starting, "Starting game...") ==
	                            Step::HandOff &&
	                    !s.controller.following(),
			"follow: the next frame hands over"))
		return false;
	// A failure stops the follow.
	s.controller.follow(true);
	s.controller.show_failure(s.rt, "lost");
	return expect(!s.controller.following() &&
	                      s.controller.follow_frame(s.rt, true, JoinScreenStage::Joining, "x") == Step::Idle,
			"follow: a failure follows no join");
}

} // namespace

int main() {
	bool ok = true;
	ok = run_each_panel_shows_its_windows() && ok;
	ok = run_the_queue_record_folds_like_retail() && ok;
	ok = run_each_stage_writes_its_line() && ok;
	ok = run_the_choice_panels_follow_the_flag_word() && ok;
	ok = run_the_controller_runs_the_screen() && ok;
	if (ok) std::printf("pre_game_menu_test: OK\n");
	return ok ? 0 : 1;
}
