// The PreMenu join screen (pre.mnu PRE_GAME_MENU): the windows each panel state
// shows, the host's S2C 0x03 queue record as the client folds it, and the
// status line each join stage writes into MESSAGES.
// [orig: UI_ShowPreGameMenuByState @0x568d10; NapiNPClientMsg_0x003 @0x425390;
//  MultiPlayer_JoinSessionStateMachine @0x56a320]

#include <runtime/inmatch/pre_game_menu.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace opennova;
using inmatch::JoinQueueRecord;
using inmatch::JoinScreenStage;
using inmatch::PreGamePanel;

namespace {

bool expect(bool cond, const char *what) {
	if (!cond) std::fprintf(stderr, "FAIL: %s\n", what);
	return cond;
}

const rtxt::File &gameerr() {
	static const rtxt::File file = [] {
		rtxt::File f;
		f.sections.push_back(rtxt::Section{"Generic Strings", 0});
		const std::vector<std::pair<std::string, std::string>> rows = {
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

int main() {
	bool ok = true;
	ok = run_each_panel_shows_its_windows() && ok;
	ok = run_the_queue_record_folds_like_retail() && ok;
	ok = run_each_stage_writes_its_line() && ok;
	if (ok) std::printf("pre_game_menu_test: OK\n");
	return ok ? 0 : 1;
}
