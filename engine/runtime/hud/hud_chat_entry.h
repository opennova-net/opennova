#pragma once

// THE CHAT ENTRY — the talk keys, the captured input line and its editing
// keys: retail's action-dispatch chat arms, Chat_BeginChatInput's capture,
// Chat_HandleInputChar's line editor (history, presets, the '\' channel
// cycle, Enter/Esc) and the facts the input-line drawer reads. The send
// itself is the embedder's (the role's C2S 0x0D sender, handed in as a
// callback exactly where retail calls the begin's stored sender); this
// class owns every witnessed rule around it.
// [orig: Input_HandleActionBinding cases 100/101/109/110/111 @0x49b989..
//  0x49bb1e, 104/105 @0x49bab3..0x49bb1e, 112 @0x49b946..0x49b984;
//  Chat_BeginChatInput @0x498060; Chat_HandleInputChar @0x49cb70;
//  Chat_ResetInputState @0x498ef0; the key router Input_ProcessKeyboardEvents
//  @0x49d1f0 hands EVERY key to the editor while g_InputCaptureMode is set
//  (@0x49d2e3 / @0x49d498), and the analog bindings pause with it
//  (Input_ProcessPlayerFrame @0x49d509)]

#include <runtime/hud/game_text_lookup.h>
#include <runtime/hud/hud_toggles.h> // HudKeyEdge (the talk rows' press latches)

#include <array>
#include <cstdint>
#include <functional>
#include <string>

namespace opennova::hud {

// The chat action dispatch ids (the talk rows' dispatch codes)
// [orig: the Input_HandleActionBinding jump-table cases].
inline constexpr int kChatDispatchTeam = 100;   // row 60 Team (Y)
inline constexpr int kChatDispatchGlobal = 101; // row 59 Global (Ctrl+T)
inline constexpr int kChatDispatchRed = 104;    // host-only red talk (no stock row)
inline constexpr int kChatDispatchBlue = 105;   // host-only blue talk (no stock row)
inline constexpr int kChatDispatchCrew = 109;   // row 62 Crew (U)
inline constexpr int kChatDispatchSquad = 110;  // row 61 Squad (Ctrl+Y)
inline constexpr int kChatDispatchLocal = 111;  // row 58 Local (T)
inline constexpr int kChatDispatchRepeat = 112; // row 57 Chat (Enter)
// The console-mode dispatch: a begin with this id keeps the capture open
// across Enter [orig: `dword_B3E318 = chatChannel == 20` @0x4981a0]. No
// caller in the image passes it (the talk arms pass 100..111, the cine
// editor 0), so the console window it gates is unreachable in stock play.
inline constexpr int kChatDispatchConsole = 20;

// The C2S 0x0D channel byte each dispatch's sender writes, -1 for none
// [orig: sub_49A840 13 @0x49a8d6; Chat_SendTeamMessage (IDB misnomer — the
//  GLOBAL sender) 1 @0x49a9af; Chat_SendGlobalMessage (IDB misnomer — the
//  TEAM sender) 2 @0x49a74f; Chat_SendSquadMessage 12 @0x49aaff;
//  Chat_SendAdminMessage (the CREW sender) 11 @0x49a816; Chat_SendAllMessage
//  4 @0x49ad1c; sub_49ABA0 5 @0x49ac4c].
int chat_dispatch_channel(int dispatch);
// The Misc gametext key of the dispatch's prompt [orig: STRMISC_TALKLOCAL
// @0x49ba62, _TALKGLOBAL @0x49ba76, _TALKTEAM @0x49ba8a, _TALKSQUAD
// @0x49ba9b, _TALKCREW @0x49baac, _TALKRED @0x49baf2, _TALKBLUE @0x49bb03];
// "" for none.
const char *chat_dispatch_prompt_key(int dispatch);
// The flood echo's packed ARGB: a refused repeat posts the raw line to the
// CHAT ring in its sender's palette entry — palette[0] white, [1] green, [3]
// light blue, all fixed by HUD_InitTeamColorTable @0x51f245..0x51f26d
// [orig: the Chat_AddMessageChannel1 calls @0x49a886 (Local [0]),
// @0x49a953 (Global [3]), @0x49a6ff (Team [1]), @0x49aaa3 (Squad [3]),
// @0x49a7c6 (Crew [1]), @0x49acc3 / @0x49abf3 (Red/Blue [3])]. 0 for none.
uint32_t chat_dispatch_flood_color(int dispatch);
// The input line's packed ARGB by the open dispatch: Team palette[1];
// Global palette[3] on an MP session peer, else [0]; Red/Blue [3]; Crew the
// auxiliary [4] (cyan); Squad the auxiliary [0] (magenta); anything else [0]
// [orig: StdCtype_Destructor (IDB misnomer — the input-line drawer)
// @0x5b8f41..0x5b8f71; the auxiliaries HUD_InitTeamColorTable @0x51f281 /
// @0x51f2a9].
uint32_t chat_input_line_color(int dispatch, bool mp_session_peer);

// The chat presets F1..F10 insert (40-byte strings the session settings copy
// from the player profile) [orig: byte_24D2370 + 40i, filled by
// Game_ApplySessionSettingsToGlobals @0x552076 from profile+0x240].
inline constexpr int kChatPresetCount = 10;
inline constexpr size_t kChatPresetChars = 39;

// What the dispatch arms read.
struct ChatEntryFacts {
	bool death_screen = false;       // [orig: g_DeathScreenActive]
	bool spawn_gate = false;         // [orig: g_SpawnSuccessGate]
	bool in_session = false;         // [orig: g_NapiNPCtx.is_in_session]
	// Set by S2C 0x25 on a client, cleared by S2C 0x0F [orig: dword_24C195C
	// — NapiNPClientMsg_GameReset @0x42284e, NapiNPClientMsg_0x00F @0x42e396].
	bool reset_hold = false;
	bool team_game = false;          // [orig: g_GameType & 0x10000]
	// The NovaWorld network type [orig: g_NapiNPCtx.transport_mode == 1].
	bool novaworld = false;
	bool authority = false;          // [orig: g_NapiNPCtx.is_authority]
	bool mp_session_peer = false;    // [orig: g_NapiNPCtx.is_mp_session_peer]
	bool has_local_player = false;   // [orig: g_LocalPlayerEntity != NULL]
	// The local player carries a def-type-1 child — rides a vehicle
	// [orig: Entity_FindChildByDefType(local, 1, 0)].
	bool in_vehicle = false;
	// The per-main-frame counter [orig: dword_A8705C — Game_TickHudFrameCounters
	// @0x434c05]; the talk debounce and the cursor blink read it.
	uint32_t frame = 0;
};

// What one send attempt did. `Sent` stamps the debounce; `Flooded` asks the
// embedder to echo the (truncated) line to the CHAT ring in the dispatch's
// flood color; `Refused` does nothing.
enum class ChatSendResult : uint8_t { Refused, Sent, Flooded };
// The begin's stored sender [orig: dword_B3E1D4]: the dispatch picks the
// C2S channel (chat_dispatch_channel); `text` is in/out — the flood check
// cuts it to 59 characters in place.
using ChatSender = std::function<ChatSendResult(int dispatch, std::string &text)>;

// Events a dispatch or key reports.
namespace chat_entry_event {
inline constexpr uint32_t kBegan = 0x1;       // the capture opened (or re-targeted)
inline constexpr uint32_t kDeniedSound = 0x2; // Crew without a vehicle: the denied tone
                                              // [orig: Sound_PlayInterfaceTriggerSet
                                              //  (dword_24E08C4) @0x49ba4b]
inline constexpr uint32_t kSubmitted = 0x4;   // Enter ran the sender
inline constexpr uint32_t kCancelled = 0x8;   // Esc closed the capture
inline constexpr uint32_t kEdited = 0x10;     // the line text changed
inline constexpr uint32_t kFloodEcho = 0x20;  // the sender flooded: echo the line
} // namespace chat_entry_event

// The talk rows the embedder samples, in catalog order (the first-match key
// scan fires the lowest row) [orig: the static catalog @0x8159cb rows 57
// "talk" (Enter, dispatch 112), 58 "ltalk" (T, 111), 59 "gtalk" (Ctrl+T,
// 101), 60 "stalk" (Y, 100), 61 "sqtalk" (Ctrl+Y, 110), 62 "ctalk" (U, 109);
// the scan @0x49d42f].
enum ChatTalkRow : int {
	kChatRowTalk,
	kChatRowLocal,
	kChatRowGlobal,
	kChatRowTeam,
	kChatRowSquad,
	kChatRowCrew,
	kChatTalkRowCount,
};
const char *chat_talk_row_token(int row);
int chat_talk_row_dispatch(int row);

// The virtual-key codes the editor reads when the translated char is 0
// [orig: the `specialKey` switch @0x49cbd7 — VK_UP 0x26, VK_DOWN 0x28,
//  VK_F1..VK_F10 0x70..0x79].
inline constexpr int kChatVkUp = 0x26;
inline constexpr int kChatVkDown = 0x28;
inline constexpr int kChatVkF1 = 0x70;

class ChatEntry {
public:
	ChatEntry() { reset_binding_state(); }

	// One chat action row's dispatch (100/101/104/105/109/110/111/112).
	uint32_t dispatch(int action, const ChatEntryFacts &facts, const GameTextLookup &gametext);
	// One frame's talk-row poll over the sampled rows (a mask of 1 <<
	// ChatTalkRow): each row's press latch steps on the ungated key state; a
	// press edge dispatches its row while gameplay input is active, no chat
	// capture is open and the editor took no key this frame — a key the
	// editor consumed (the Enter that submitted) never also re-dispatches,
	// as retail's router hands each key to exactly one of the two
	// [orig: Input_ProcessKeyboardEvents @0x49d2e3 — the capture branch
	// @0x49d498 vs the special-keys/binding scan @0x49d2fb..0x49d488].
	uint32_t poll_rows(uint32_t rows_down, bool active, bool chorded,
			const ChatEntryFacts &facts, const GameTextLookup &gametext);
	// One key event while the capture is open: `key_char` the translated
	// character (0 for none), `vk` the virtual key. Every key reaches here
	// while capturing(); the embedder routes nothing else.
	uint32_t key(int key_char, int vk, const ChatEntryFacts &facts,
			const GameTextLookup &gametext, const ChatSender &send);
	// [orig: Chat_ResetInputState @0x498ef0]
	void reset();

	bool capturing() const { return capture_mode_ != 0; }
	const std::string &prompt() const { return prompt_; }
	const std::string &text() const { return text_; }
	int open_dispatch() const { return open_dispatch_; }
	bool console() const { return console_; }
	int last_action() const { return action_id_; }
	// The last flood echo (kFloodEcho): the line and its dispatch.
	const std::string &echo_text() const { return echo_text_; }
	int echo_dispatch() const { return echo_dispatch_; }
	void set_preset(int index, const std::string &text);
	const std::array<std::string, 5> &history() const { return history_; }

private:
	uint32_t begin(const std::string &prompt, int max_len, int dispatch);
	void reset_binding_state();

	int capture_mode_ = 0;          // [orig: g_InputCaptureMode]
	std::string prompt_;            // [orig: byte_B3E240]
	std::string text_;              // [orig: byte_B3E1DC]
	int max_len_ = 100;             // [orig: dword_B3E30C]
	int open_dispatch_ = -1;        // [orig: dword_B3E314]
	bool console_ = false;          // [orig: dword_B3E318]
	int history_index_ = -1;        // [orig: dword_B3E310]
	std::array<std::string, 5> history_{}; // [orig: byte_B3DFE0, 5 x 100]
	std::string last_sent_;         // [orig: unk_B3E2A4]
	int action_id_ = kChatDispatchLocal; // [orig: g_InputActionId]
	uint32_t last_send_frame_ = 0;  // [orig: dword_B3BBCC]
	std::array<std::string, kChatPresetCount> presets_{};
	std::string echo_text_;
	int echo_dispatch_ = -1;
	std::array<HudKeyEdge, kChatTalkRowCount> rows_{};
	bool key_consumed_ = false;
};

} // namespace opennova::hud
