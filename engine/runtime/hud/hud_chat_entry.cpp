// The chat entry — see hud_chat_entry.h.
#include <runtime/hud/hud_chat_entry.h>

#include <runtime/hud/feed_format.h> // the fixed HUD palette immediates
#include <base/io/strutil.h>

namespace opennova::hud {

int chat_dispatch_channel(int dispatch) {
	switch (dispatch) {
		case kChatDispatchLocal: return 13;  // [orig: sub_49A840 @0x49a8d6]
		case kChatDispatchGlobal: return 1;  // [orig: @0x49a9af]
		case kChatDispatchTeam: return 2;    // [orig: @0x49a74f]
		case kChatDispatchSquad: return 12;  // [orig: @0x49aaff]
		case kChatDispatchCrew: return 11;   // [orig: @0x49a816]
		case kChatDispatchRed: return 4;     // [orig: @0x49ad1c]
		case kChatDispatchBlue: return 5;    // [orig: @0x49ac4c]
		default: return -1;
	}
}

const char *chat_dispatch_prompt_key(int dispatch) {
	switch (dispatch) {
		case kChatDispatchLocal: return "STRMISC_TALKLOCAL";   // [orig: @0x49ba62]
		case kChatDispatchGlobal: return "STRMISC_TALKGLOBAL"; // [orig: @0x49ba76]
		case kChatDispatchTeam: return "STRMISC_TALKTEAM";     // [orig: @0x49ba8a]
		case kChatDispatchSquad: return "STRMISC_TALKSQUAD";   // [orig: @0x49ba9b]
		case kChatDispatchCrew: return "STRMISC_TALKCREW";     // [orig: @0x49baac]
		case kChatDispatchRed: return "STRMISC_TALKRED";       // [orig: @0x49baf2]
		case kChatDispatchBlue: return "STRMISC_TALKBLUE";     // [orig: @0x49bb03]
		default: return "";
	}
}

uint32_t chat_dispatch_flood_color(int dispatch) {
	switch (dispatch) {
		case kChatDispatchLocal: return kHudColorWhite;      // palette[0] @0x49a886
		case kChatDispatchTeam:                              // palette[1] @0x49a6ff
		case kChatDispatchCrew: return kHudColorGreen;       // palette[1] @0x49a7c6
		case kChatDispatchGlobal:                            // palette[3] @0x49a953
		case kChatDispatchSquad:                             // palette[3] @0x49aaa3
		case kChatDispatchRed:                               // palette[3] @0x49acc3
		case kChatDispatchBlue: return kHudColorLightBlue;   // palette[3] @0x49abf3
		default: return 0;
	}
}

uint32_t chat_input_line_color(int dispatch, bool mp_session_peer) {
	// [orig: StdCtype_Destructor @0x5b8f41 — switch (Input_GetMountedEntityId()
	// = dword_B3E314)]
	switch (dispatch) {
		case kChatDispatchTeam: return kHudColorGreen;          // palette[1] @0x5b8f48
		case kChatDispatchGlobal:                                // @0x5b8f57
			return mp_session_peer ? kHudColorLightBlue : kHudColorWhite;
		case kChatDispatchRed:
		case kChatDispatchBlue: return kHudColorLightBlue;      // palette[3] @0x5b8f59
		case kChatDispatchCrew: return kHudColorCyan;           // aux[4] @0x5b8f61
		case kChatDispatchSquad: return kHudColorMagenta;       // aux[0] @0x5b8f69
		default: return kHudColorWhite;                         // palette[0] @0x5b8f71
	}
}

namespace {

// [orig: the catalog rows 57..62 @0x8159cb — config tokens and dispatch codes]
struct TalkRowSpec {
	const char *token;
	int dispatch;
};
constexpr TalkRowSpec kTalkRows[kChatTalkRowCount] = {
	{ "talk", kChatDispatchRepeat },
	{ "ltalk", kChatDispatchLocal },
	{ "gtalk", kChatDispatchGlobal },
	{ "stalk", kChatDispatchTeam },
	{ "sqtalk", kChatDispatchSquad },
	{ "ctalk", kChatDispatchCrew },
};

} // namespace

const char *chat_talk_row_token(int row) {
	return row >= 0 && row < kChatTalkRowCount ? kTalkRows[row].token : "";
}

int chat_talk_row_dispatch(int row) {
	return row >= 0 && row < kChatTalkRowCount ? kTalkRows[row].dispatch : -1;
}

uint32_t ChatEntry::poll_rows(uint32_t rows_down, bool active, bool chorded,
		const ChatEntryFacts &facts, const GameTextLookup &gametext) {
	const bool open = active && !capturing() && !key_consumed_;
	key_consumed_ = false;
	uint32_t events = 0;
	for (int row = 0; row < kChatTalkRowCount; ++row) {
		const bool down = (rows_down & (1u << row)) != 0u;
		// The latch follows the ungated key state; once a row opened the
		// capture, the later rows' keys belong to the editor.
		if (rows_[static_cast<size_t>(row)].step(down, open, chorded) && !capturing())
			events |= dispatch(kTalkRows[row].dispatch, facts, gametext);
	}
	return events;
}

void ChatEntry::reset_binding_state() {
	// [orig: Input_InitBindingSystem @0x49a65f..0x49a699 — capture 0, the
	// buffers empty, max 100, history / dispatch -1, g_InputActionId = 111]
	reset();
	action_id_ = kChatDispatchLocal;
}

void ChatEntry::reset() {
	// [orig: Chat_ResetInputState @0x498ef0]
	capture_mode_ = 0;
	prompt_.clear();
	text_.clear();
	max_len_ = 100;
	history_index_ = -1;
	open_dispatch_ = -1;
	console_ = false;
}

void ChatEntry::set_preset(int index, const std::string &text) {
	if (index < 0 || index >= kChatPresetCount) return;
	presets_[static_cast<size_t>(index)] = text.substr(0, kChatPresetChars);
}

uint32_t ChatEntry::begin(const std::string &prompt, int max_len, int dispatch) {
	// [orig: Chat_BeginChatInput @0x498060 — the talk arms pass (prompt, NULL
	// initial text, sender, 60, dispatch)]
	if (max_len - 1 <= 0) return 0; // [orig: @0x49808b]
	const int clamped = max_len - 1 > 100 ? 100 : max_len; // [orig: @0x498094]
	max_len_ = clamped - 1;                                 // [orig: @0x4980a5]
	if (capture_mode_ == 1) {
		// A re-dispatch while open keeps the typed text, cut to the new cap
		// [orig: `byte_B3E1DB[clamped] = 0` @0x4980c3].
		if (static_cast<int>(text_.size()) >= clamped)
			text_.resize(static_cast<size_t>(clamped - 1));
	} else {
		prompt_.clear(); // [orig: the 100-byte clears @0x4980cb..0x4980d6]
		text_.clear();
	}
	prompt_ = prompt;             // [orig: the prefix copy @0x4980f3]
	open_dispatch_ = dispatch;    // [orig: @0x498191]
	capture_mode_ = 1;            // [orig: @0x498196]
	console_ = dispatch == kChatDispatchConsole; // [orig: @0x4981a0]
	return chat_entry_event::kBegan;
}

uint32_t ChatEntry::dispatch(int action, const ChatEntryFacts &f, const GameTextLookup &gametext) {
	using namespace chat_entry_event;
	switch (action) {
		case kChatDispatchRepeat: {
			// Enter re-dispatches the last talk row; a remembered Crew without
			// a vehicle falls back to Local [orig: @0x49b946..0x49b96e].
			if (action_id_ == kChatDispatchCrew && (!f.has_local_player || !f.in_vehicle))
				action_id_ = kChatDispatchLocal;
			return dispatch(action_id_, f, gametext); // [orig: @0x49b97c]
		}
		case kChatDispatchTeam:
		case kChatDispatchGlobal:
		case kChatDispatchCrew:
		case kChatDispatchSquad:
		case kChatDispatchLocal: {
			// [orig: @0x49b989..0x49b9b4 — `death && !spawn gate`, in session,
			// !dword_24C195C]
			if (f.death_screen && !f.spawn_gate) return 0;
			if (!f.in_session) return 0;
			if (f.reset_hold) return 0;
			// Team talk needs a team game [orig: `g_GameType & 0x10000`
			// @0x49b9bf]; squad talk a NovaWorld session [orig: `transport_mode
			// == 1` @0x49ba13]; crew talk a living player aboard a vehicle, or
			// the denied tone [orig: @0x49ba27..0x49ba4b].
			if (action == kChatDispatchTeam && !f.team_game) return 0;
			if (action == kChatDispatchSquad && !f.novaworld) return 0;
			if (action == kChatDispatchCrew) {
				if (f.death_screen) return 0;
				if (!f.has_local_player || !f.in_vehicle) return kDeniedSound;
			}
			// The talk debounce: more than 8 main frames since the last send
			// [orig: `dword_A8705C - dword_B3BBCC > 8` unsigned @0x49b9d6..0x49b9e8].
			if (static_cast<uint32_t>(f.frame - last_send_frame_) <= 8u) return 0;
			action_id_ = action; // [orig: `mov g_InputActionId, edi` @0x49b9f4]
			return begin(game_text(gametext, "Misc", chat_dispatch_prompt_key(action), ""), 60,
					action);
		}
		case kChatDispatchRed:
		case kChatDispatchBlue:
			// The host-only pair: no debounce, no reset hold, no remembered
			// action [orig: @0x49bab3..0x49badd — `death && !spawn gate`, in
			// session, is_authority].
			if (f.death_screen && !f.spawn_gate) return 0;
			if (!f.in_session || !f.authority) return 0;
			return begin(game_text(gametext, "Misc", chat_dispatch_prompt_key(action), ""), 60,
					action);
		default:
			return 0;
	}
}

uint32_t ChatEntry::key(int key_char, int vk, const ChatEntryFacts &f,
		const GameTextLookup &gametext, const ChatSender &send) {
	using namespace chat_entry_event;
	// [orig: Chat_HandleInputChar @0x49cb70 — the capture-mode-1 arm]
	if (capture_mode_ != 1) return 0;
	key_consumed_ = true;
	const int input_len = static_cast<int>(text_.size());
	switch (key_char) {
		case 0:
			// The non-character keys [orig: the specialKey switch @0x49cbd7].
			if (vk == kChatVkUp) {
				text_ = last_sent_; // the last sent line [orig: @0x49cd18..0x49cd2d]
				return kEdited;
			}
			if (vk == kChatVkDown) {
				// The five-line ring, walked newest first and wrapping
				// [orig: ++dword_B3E310, >= 5 -> 0 @0x49cd3a..0x49cd4a].
				if (++history_index_ >= 5) history_index_ = 0;
				text_ = history_[static_cast<size_t>(history_index_)];
				return kEdited;
			}
			if (vk >= kChatVkF1 && vk < kChatVkF1 + kChatPresetCount) {
				// F1..F10 replace the line with a preset [orig: 'p'..'y'
				// @0x49cbde..0x49cd11 copying byte_24D2370 + 40i].
				text_ = presets_[static_cast<size_t>(vk - kChatVkF1)];
				return kEdited;
			}
			return 0; // [orig: the default return @0x49cbd7]
		case 8:
			// Backspace [orig: @0x49cd75..0x49cd7c]
			if (input_len != 0) text_.pop_back();
			return kEdited;
		case 13: {
			// Enter [orig: @0x49cd87..0x49ce6a]: the capture stays open only in
			// console mode; a line not already in the ring shifts it down and
			// lands newest; the line becomes the Up recall; the sender runs;
			// the line empties.
			capture_mode_ = console_ ? 1 : 0;
			bool known = false;
			for (const std::string &h : history_)
				if (strutil::iequals(h, text_)) { known = true; break; } // [orig: _stricmp @0x49cdb0]
			if (!known) {
				for (size_t i = history_.size() - 1; i > 0; --i) history_[i] = history_[i - 1];
				history_[0] = text_; // [orig: @0x49cdc6..0x49cdfd]
			}
			last_sent_ = text_; // [orig: @0x49cdff..0x49ce11]
			uint32_t events = kSubmitted;
			std::string line = text_;
			const int dispatch_id = open_dispatch_;
			// [orig: dword_B3E1D4(&byte_B3E1DC) @0x49ce18]
			if (send) {
				const ChatSendResult result = send(dispatch_id, line);
				// Each sender stamps the debounce on a queued line
				// [orig: `dword_B3BBCC = dword_A8705C` @0x49a8e5 / @0x49a9be / ...].
				if (result == ChatSendResult::Sent) last_send_frame_ = f.frame;
				if (result == ChatSendResult::Flooded) {
					echo_text_ = line;
					echo_dispatch_ = dispatch_id;
					events |= kFloodEcho;
				}
			}
			prompt_.clear(); // [orig: @0x49ce27..0x49ce35]
			text_.clear();
			max_len_ = 100;
			history_index_ = -1;
			if (!console_) {
				capture_mode_ = 0; // [orig: @0x49ce43..0x49ce60]
				console_ = false;
				open_dispatch_ = -1;
			}
			return events;
		}
		case 27:
			// Escape: the sender with no line (it posts nothing), then the
			// reset [orig: dword_B3E1D4(0) @0x49ce77; Chat_ResetInputState
			// @0x49ce84].
			reset();
			return kCancelled;
		case '\\':
			// The channel cycle re-dispatches through the talk arms (their
			// gates and debounce apply again); any other open dispatch types
			// the backslash [orig: @0x49ce89..0x49cf22].
			switch (open_dispatch_) {
				case kChatDispatchTeam:
					return dispatch(kChatDispatchGlobal, f, gametext);
				case kChatDispatchGlobal:
					if (!f.death_screen && f.has_local_player && f.in_vehicle)
						return dispatch(kChatDispatchCrew, f, gametext);
					return dispatch(kChatDispatchLocal, f, gametext);
				case kChatDispatchCrew:
					return dispatch(kChatDispatchLocal, f, gametext);
				case kChatDispatchLocal:
					return dispatch(f.team_game ? kChatDispatchTeam : kChatDispatchGlobal, f,
							gametext);
				default:
					break;
			}
			break;
		default:
			break;
	}
	// Typing: a printable character while the line is under its cap
	// [orig: `input_len < dword_B3E30C && !iscntrl(keyChar)` @0x49cf30].
	const int c = key_char & 0xFF;
	if (input_len < max_len_ && c >= 0x20 && c != 0x7F) {
		text_.push_back(static_cast<char>(c));
		return kEdited;
	}
	return 0;
}

} // namespace opennova::hud
