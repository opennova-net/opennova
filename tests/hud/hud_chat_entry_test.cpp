// The talk keys and the captured chat line [orig: Input_HandleActionBinding
// cases 100..112 @0x49b946..0x49bb1e; Chat_BeginChatInput @0x498060;
// Chat_HandleInputChar @0x49cb70; Chat_ResetInputState @0x498ef0]: the
// dispatch gates and debounce, the capture's begin/re-target, the line
// editor (typing, Backspace, the history ring and Up recall, the presets,
// the '\' channel cycle, Enter's send, Esc), the channel bytes and colors,
// and the talk-row poll's one-consumer rule.
#include <cstdio>
#include <string>
#include <vector>

#include <runtime/hud/feed_format.h>
#include <runtime/hud/hud_chat_entry.h>

using namespace opennova::hud;

static int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

ChatEntryFacts live(uint32_t frame = 100) {
	ChatEntryFacts f;
	f.in_session = true;
	f.mp_session_peer = true;
	f.team_game = true;
	f.has_local_player = true;
	f.frame = frame;
	return f;
}

const GameTextLookup kText = [](const char *section, const char *key,
		const char *fallback) -> std::string {
	if (std::string(section) != "Misc") return fallback;
	return std::string("<") + key + ">";
};

struct Sent {
	int dispatch = -1;
	std::string text;
};

// Types a string one character at a time.
void type(ChatEntry &c, const char *s, const ChatEntryFacts &f) {
	for (const char *p = s; *p; ++p) c.key(static_cast<unsigned char>(*p), 0, f, kText, nullptr);
}

void test_channels_and_colors() {
	CHECK(chat_dispatch_channel(kChatDispatchLocal) == 13);
	CHECK(chat_dispatch_channel(kChatDispatchGlobal) == 1);
	CHECK(chat_dispatch_channel(kChatDispatchTeam) == 2);
	CHECK(chat_dispatch_channel(kChatDispatchSquad) == 12);
	CHECK(chat_dispatch_channel(kChatDispatchCrew) == 11);
	CHECK(chat_dispatch_channel(kChatDispatchRed) == 4);
	CHECK(chat_dispatch_channel(kChatDispatchBlue) == 5);
	CHECK(chat_dispatch_channel(kChatDispatchRepeat) == -1);
	CHECK(std::string(chat_dispatch_prompt_key(kChatDispatchLocal)) == "STRMISC_TALKLOCAL");
	CHECK(std::string(chat_dispatch_prompt_key(kChatDispatchTeam)) == "STRMISC_TALKTEAM");
	CHECK(chat_dispatch_flood_color(kChatDispatchLocal) == kHudColorWhite);
	CHECK(chat_dispatch_flood_color(kChatDispatchTeam) == kHudColorGreen);
	CHECK(chat_dispatch_flood_color(kChatDispatchCrew) == kHudColorGreen);
	CHECK(chat_dispatch_flood_color(kChatDispatchGlobal) == kHudColorLightBlue);
	CHECK(chat_dispatch_flood_color(kChatDispatchSquad) == kHudColorLightBlue);
	CHECK(chat_input_line_color(kChatDispatchTeam, true) == kHudColorGreen);
	CHECK(chat_input_line_color(kChatDispatchGlobal, true) == kHudColorLightBlue);
	CHECK(chat_input_line_color(kChatDispatchGlobal, false) == kHudColorWhite);
	CHECK(chat_input_line_color(kChatDispatchCrew, true) == kHudColorCyan);
	CHECK(chat_input_line_color(kChatDispatchSquad, true) == kHudColorMagenta);
	CHECK(chat_input_line_color(kChatDispatchLocal, true) == kHudColorWhite);
	CHECK(std::string(chat_talk_row_token(kChatRowTalk)) == "talk");
	CHECK(chat_talk_row_dispatch(kChatRowGlobal) == kChatDispatchGlobal);
	CHECK(chat_talk_row_dispatch(kChatRowCrew) == kChatDispatchCrew);
}

// The talk arms' gates [orig: @0x49b989..0x49ba4b].
void test_dispatch_gates() {
	ChatEntry c;
	ChatEntryFacts f = live();
	// Out of a session nothing opens.
	f.in_session = false;
	CHECK(c.dispatch(kChatDispatchLocal, f, kText) == 0 && !c.capturing());
	f = live();
	// Dead without the round-over latch: closed; the latch reopens it.
	f.death_screen = true;
	CHECK(c.dispatch(kChatDispatchLocal, f, kText) == 0);
	f.spawn_gate = true;
	CHECK(c.dispatch(kChatDispatchLocal, f, kText) == chat_entry_event::kBegan);
	CHECK(c.capturing() && c.prompt() == "<STRMISC_TALKLOCAL>" &&
			c.open_dispatch() == kChatDispatchLocal);
	c.reset();
	// The reset hold closes every talk arm.
	f = live();
	f.reset_hold = true;
	CHECK(c.dispatch(kChatDispatchGlobal, f, kText) == 0);
	// Team talk needs a team game; squad talk a NovaWorld session.
	f = live();
	f.team_game = false;
	CHECK(c.dispatch(kChatDispatchTeam, f, kText) == 0);
	CHECK(c.dispatch(kChatDispatchSquad, f, kText) == 0);
	f.novaworld = true;
	CHECK(c.dispatch(kChatDispatchSquad, f, kText) == chat_entry_event::kBegan);
	c.reset();
	// Crew talk: dead is refused; afoot plays the denied tone.
	f = live();
	CHECK(c.dispatch(kChatDispatchCrew, f, kText) == chat_entry_event::kDeniedSound);
	CHECK(!c.capturing());
	f.in_vehicle = true;
	CHECK(c.dispatch(kChatDispatchCrew, f, kText) == chat_entry_event::kBegan);
	c.reset();
	// The host-only pair needs the authority and skips the reset hold.
	f = live();
	f.reset_hold = true;
	CHECK(c.dispatch(kChatDispatchRed, f, kText) == 0);
	f.authority = true;
	CHECK(c.dispatch(kChatDispatchRed, f, kText) == chat_entry_event::kBegan);
	CHECK(c.prompt() == "<STRMISC_TALKRED>");
}

// Enter re-dispatches the last talk row; a remembered Crew without a vehicle
// falls back to Local; the initial row is Local [orig: @0x49b946; the
// g_InputActionId seed 0x6F @0x49a699].
void test_repeat_row() {
	ChatEntry c;
	ChatEntryFacts f = live();
	CHECK(c.dispatch(kChatDispatchRepeat, f, kText) == chat_entry_event::kBegan);
	CHECK(c.open_dispatch() == kChatDispatchLocal);
	c.reset();
	f.in_vehicle = true;
	CHECK(c.dispatch(kChatDispatchCrew, f, kText) == chat_entry_event::kBegan);
	c.reset();
	CHECK(c.last_action() == kChatDispatchCrew);
	f.in_vehicle = false;
	CHECK(c.dispatch(kChatDispatchRepeat, f, kText) == chat_entry_event::kBegan);
	CHECK(c.open_dispatch() == kChatDispatchLocal && c.last_action() == kChatDispatchLocal);
}

// Typing, Backspace, the 59-character cap and Esc [orig: @0x49cd75,
// @0x49cf30, the cap dword_B3E30C = 60 - 1 @0x4980a5; Esc @0x49ce77].
void test_line_editing() {
	ChatEntry c;
	const ChatEntryFacts f = live();
	c.dispatch(kChatDispatchLocal, f, kText);
	type(c, "hi there", f);
	CHECK(c.text() == "hi there");
	c.key(8, 0, f, kText, nullptr);
	CHECK(c.text() == "hi ther");
	// Control characters never type.
	c.key(9, 0, f, kText, nullptr);
	CHECK(c.text() == "hi ther");
	// The cap: 59 characters.
	c.key(27, 0, f, kText, nullptr);
	CHECK(!c.capturing() && c.text().empty());
	c.dispatch(kChatDispatchLocal, live(200), kText);
	for (int i = 0; i < 70; ++i) c.key('x', 0, f, kText, nullptr);
	CHECK(c.text().size() == 59);
	// Esc runs no send and closes.
	int sends = 0;
	const ChatSender count = [&sends](int, std::string &) {
		++sends;
		return ChatSendResult::Sent;
	};
	CHECK(c.key(27, 0, f, kText, count) == chat_entry_event::kCancelled);
	CHECK(sends == 0 && !c.capturing());
}

// Enter: the sender runs with the dispatch and line, the line joins the
// history ring (newest first, a repeat not re-added), Up recalls the last
// sent line, Down walks the ring and wraps; a sent line stamps the talk
// debounce (8 frames) [orig: @0x49cd87..0x49ce6a; @0x49cd18; @0x49cd3a;
// @0x49b9d6..0x49b9e8].
void test_submit_history_and_debounce() {
	ChatEntry c;
	std::vector<Sent> sent;
	const ChatSender send = [&sent](int dispatch, std::string &text) {
		sent.push_back({dispatch, text});
		return ChatSendResult::Sent;
	};
	ChatEntryFacts f = live(100);
	c.dispatch(kChatDispatchTeam, f, kText);
	type(c, "one", f);
	CHECK(c.key(13, 0, f, kText, send) == chat_entry_event::kSubmitted);
	CHECK(sent.size() == 1 && sent[0].dispatch == kChatDispatchTeam && sent[0].text == "one");
	CHECK(!c.capturing() && c.text().empty() && c.prompt().empty());
	CHECK(c.history()[0] == "one");
	// The debounce: within 8 frames of the send nothing opens.
	CHECK(c.dispatch(kChatDispatchTeam, live(108), kText) == 0);
	f = live(109);
	CHECK(c.dispatch(kChatDispatchTeam, f, kText) == chat_entry_event::kBegan);
	type(c, "two", f);
	c.key(13, 0, f, kText, send);
	// A case-insensitive repeat does not re-enter the ring.
	f = live(200);
	c.dispatch(kChatDispatchTeam, f, kText);
	type(c, "ONE", f);
	c.key(13, 0, f, kText, send);
	CHECK(c.history()[0] == "two" && c.history()[1] == "one" && c.history()[2].empty());
	// Up recalls the last SENT line; Down walks the ring from the newest.
	f = live(300);
	c.dispatch(kChatDispatchTeam, f, kText);
	c.key(0, kChatVkUp, f, kText, send);
	CHECK(c.text() == "ONE");
	c.key(0, kChatVkDown, f, kText, send);
	CHECK(c.text() == "two");
	c.key(0, kChatVkDown, f, kText, send);
	CHECK(c.text() == "one");
	for (int i = 0; i < 3; ++i) c.key(0, kChatVkDown, f, kText, send);
	CHECK(c.text().empty()); // slot 4
	c.key(0, kChatVkDown, f, kText, send);
	CHECK(c.text() == "two"); // wrapped past slot 4 back to 0
	// A refused send stamps no debounce.
	ChatEntry r;
	const ChatSender refuse = [](int, std::string &) { return ChatSendResult::Refused; };
	f = live(500);
	r.dispatch(kChatDispatchLocal, f, kText);
	type(r, "x", f);
	r.key(13, 0, f, kText, refuse);
	CHECK(r.dispatch(kChatDispatchLocal, live(501), kText) == chat_entry_event::kBegan);
}

// A flooded send reports the echo with the line the sender cut and its
// dispatch [orig: the senders' Chat_AddMessageChannel1 else-arms].
void test_flood_echo() {
	ChatEntry c;
	const ChatSender flood = [](int, std::string &text) {
		text.resize(3);
		return ChatSendResult::Flooded;
	};
	const ChatEntryFacts f = live();
	c.dispatch(kChatDispatchGlobal, f, kText);
	type(c, "spam!", f);
	const uint32_t events = c.key(13, 0, f, kText, flood);
	CHECK((events & chat_entry_event::kFloodEcho) != 0);
	CHECK(c.echo_text() == "spa" && c.echo_dispatch() == kChatDispatchGlobal);
}

// The presets replace the line (F1..F10); an unknown special key does
// nothing [orig: @0x49cbde..0x49cd11; the default return].
void test_presets() {
	ChatEntry c;
	c.set_preset(0, "Need backup!");
	c.set_preset(9, "Last one");
	const ChatEntryFacts f = live();
	c.dispatch(kChatDispatchLocal, f, kText);
	type(c, "abc", f);
	c.key(0, kChatVkF1, f, kText, nullptr);
	CHECK(c.text() == "Need backup!");
	c.key(0, kChatVkF1 + 9, f, kText, nullptr);
	CHECK(c.text() == "Last one");
	c.key(0, kChatVkF1 + 1, f, kText, nullptr);
	CHECK(c.text().empty());
	CHECK(c.key(0, 0x7A, f, kText, nullptr) == 0); // F11
}

// The '\' cycle re-targets the open line through the talk arms, keeping the
// typed text: Team -> Global -> (Crew aboard a vehicle | Local) -> Local ->
// (Team in a team game | Global); any other dispatch types the backslash
// [orig: @0x49ce89..0x49cf22; the re-dispatch keeps the text @0x4980aa].
void test_channel_cycle() {
	ChatEntry c;
	ChatEntryFacts f = live();
	c.dispatch(kChatDispatchTeam, f, kText);
	type(c, "go", f);
	c.key('\\', 0, f, kText, nullptr);
	CHECK(c.open_dispatch() == kChatDispatchGlobal && c.text() == "go");
	CHECK(c.prompt() == "<STRMISC_TALKGLOBAL>");
	c.key('\\', 0, f, kText, nullptr);
	CHECK(c.open_dispatch() == kChatDispatchLocal); // afoot
	c.key('\\', 0, f, kText, nullptr);
	CHECK(c.open_dispatch() == kChatDispatchTeam); // a team game
	c.key('\\', 0, f, kText, nullptr);
	CHECK(c.open_dispatch() == kChatDispatchGlobal);
	f.in_vehicle = true;
	c.key('\\', 0, f, kText, nullptr);
	CHECK(c.open_dispatch() == kChatDispatchCrew);
	c.key('\\', 0, f, kText, nullptr);
	CHECK(c.open_dispatch() == kChatDispatchLocal);
	f.team_game = false;
	c.key('\\', 0, f, kText, nullptr);
	CHECK(c.open_dispatch() == kChatDispatchGlobal && c.text() == "go");
	// Squad has no cycle arm: the backslash types.
	c.reset();
	f.novaworld = true;
	c.dispatch(kChatDispatchSquad, f, kText);
	c.key('\\', 0, f, kText, nullptr);
	CHECK(c.text() == "\\" && c.open_dispatch() == kChatDispatchSquad);
}

// The talk-row poll: a press edge dispatches its row; rows after an opened
// one wait; a key the editor consumed never also dispatches (the Enter that
// submitted), and held rows need a fresh press.
void test_poll_rows() {
	ChatEntry c;
	const ChatEntryFacts f = live();
	const uint32_t local = 1u << kChatRowLocal;
	const uint32_t enter = 1u << kChatRowTalk;
	CHECK(c.poll_rows(local, true, false, f, kText) == chat_entry_event::kBegan);
	CHECK(c.open_dispatch() == kChatDispatchLocal);
	// Held: no second edge; capturing: rows are the editor's.
	CHECK(c.poll_rows(local, true, false, f, kText) == 0);
	// Enter reaches the editor, submits; the same frame's poll sees Enter
	// down but must not reopen.
	c.key('x', 0, f, kText, nullptr);
	c.key(13, 0, f, kText, [](int, std::string &) { return ChatSendResult::Sent; });
	CHECK(!c.capturing());
	CHECK(c.poll_rows(enter, true, false, f, kText) == 0 && !c.capturing());
	// Released, then pressed after the debounce: the repeat row reopens Local.
	c.poll_rows(0, true, false, live(300), kText);
	CHECK(c.poll_rows(enter, true, false, live(301), kText) == chat_entry_event::kBegan);
	c.reset();
	// Inactive gameplay or a Shift/Alt chord never dispatches.
	ChatEntry d;
	d.poll_rows(0, true, false, f, kText);
	CHECK(d.poll_rows(local, false, false, f, kText) == 0);
	d.poll_rows(0, true, false, f, kText);
	CHECK(d.poll_rows(local, true, true, f, kText) == 0);
}

} // namespace

int main() {
	test_channels_and_colors();
	test_dispatch_gates();
	test_repeat_row();
	test_line_editing();
	test_submit_history_and_debounce();
	test_flood_echo();
	test_presets();
	test_channel_cycle();
	test_poll_rows();
	if (failures == 0) std::printf("hud_chat_entry_test: all passed\n");
	return failures == 0 ? 0 : 1;
}
