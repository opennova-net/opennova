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
inline constexpr int kChatDispatchRed = 104;    // row 90 talkred, the host's (no default key)
inline constexpr int kChatDispatchBlue = 105;   // row 91 talkblue, the host's (no default key)
inline constexpr int kChatDispatchCrew = 109;   // row 62 Crew (U)
inline constexpr int kChatDispatchSquad = 110;  // row 61 Squad (Ctrl+Y)
inline constexpr int kChatDispatchLocal = 111;  // row 58 Local (T)
inline constexpr int kChatDispatchRepeat = 112; // row 57 Chat (Enter)
// The command entry: input actions 1 (row 71 "command", the backquote) and 20
// (row 72 "command2", Ctrl+backquote) open the capture with the STRMISC_ENTERCMD
// prompt and the console command as its sender; action 20's begin passes its own
// code, so its capture stays open across Enter (the console mode).
// [orig: Input_HandleActionBinding cases 1, 20 @0x49AEF7..0x49AF10 ->
//  Chat_BeginChatInput(prompt, NULL, j_Client_ProcessConsoleCommand, 0x3C, action)
//  @0x49bb16; `dword_B3E318 = chatChannel == 20` @0x4981a0]
inline constexpr int kChatDispatchCommand = 1;
inline constexpr int kChatDispatchConsole = 20;

// The C2S 0x0D channel byte each dispatch's sender writes, -1 for none
// [orig: Chat_SendLocalMessage 13 @0x49a8d6; Chat_SendGlobalMessage 1 @0x49a9af;
//  Chat_SendTeamMessage 2 @0x49a74f; Chat_SendSquadMessage 12 @0x49aaff;
//  Chat_SendAdminMessage (the CREW sender) 11 @0x49a816; Chat_SendAllMessage
//  4 @0x49ad1c; Chat_SendBlueTeamMessage 5 @0x49ac4c].
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
// [orig: HUD_DrawChatInputLine
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
	bool round_over = false;         // [orig: g_RoundOver]
	bool in_session = false;         // [orig: g_NapiNPCtx.is_in_session]
	// Set by S2C 0x25 on a client, cleared by S2C 0x0F [orig: dword_24C195C
	// — NapiNPClientMsg_GameReset @0x42284e, NapiNPClientMsg_0x00F @0x42e396].
	bool reset_hold = false;
	bool team_game = false;          // [orig: g_GameType & 0x10000]
	// The NovaWorld network type [orig: g_NapiNPCtx.network_type == 1].
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
// flood color; `Refused` does nothing. `Broadcast` is the Global sender's
// authority arm on a host with no client of its own: the line went out to
// every player, and that arm stamps no debounce [orig: Chat_SendGlobalMessage
// @0x49a9da..0x49aa2a; the stamp
// @0x49a9be is the peer arm's alone].
enum class ChatSendResult : uint8_t { Refused, Sent, Flooded, Broadcast };
// The begin's stored sender [orig: dword_B3E1D4]: the dispatch picks the
// C2S channel (chat_dispatch_channel); `text` is in/out — the flood check
// cuts it to 59 characters in place. The command entry's dispatches (1, 20)
// store the console command instead (Client_ProcessConsoleCommand), which
// stamps no debounce: its sender reports Refused.
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
// The row released Caps Lock: a talk row past its arm's gates (ahead of the debounce, so a
// debounced press releases it too) and the command rows, never the host-only pair. The
// embedder's keyboard device turns the toggle off when it is on (a VK_CAPITAL press and
// release, the release KEYEVENTF_KEYUP with time 10, then the window's messages).
// [orig: Input_HandleActionBinding @0x49AD40 - the talk arms' join @0x49B9CF (the team arm
//  past its team-game test @0x49B9BF, the squad's past its NovaWorld test @0x49BA20, the
//  crew's past its vehicle test @0x49BA42, the others' @0x49BA25), the call @0x49B9D1 ahead
//  of the debounce @0x49B9D6; the command arm, cases 1 and 20, @0x49AEF7..0x49AEF9;
//  Input_ReleaseCapsLockIfActive @0x7611A0 (the VK_CAPITAL byte compare @0x7611C6, SendInput
//  @0x76121B, the pump @0x761224)]
inline constexpr uint32_t kCapsLockRelease = 0x40;
} // namespace chat_entry_event

// The talk rows the embedder samples, in catalog order (the first-match key
// scan fires the lowest row) [orig: the static catalog @0x8159cb rows 57
// "talk" (Enter, dispatch 112), 58 "ltalk" (T, 111), 59 "gtalk" (Ctrl+T,
// 101), 60 "stalk" (Y, 100), 61 "sqtalk" (Ctrl+Y, 110), 62 "ctalk" (U, 109),
// and the command entry's 71 "command" (backquote, 1) and 72 "command2"
// (Ctrl+backquote, 20); the scan @0x49d42f]. Then the host's two side talks, rows 90
// "talkred" (Indonesian Talk, dispatch 104) and 91 "talkblue" (Joint Ops Talk, 105), flags
// 0x04000000 and no default key: a keylist binding or a typed token queues their records,
// which open the side's line on a session's authority (ChatEntry::dispatch)
// (D-NET-NEW-LI-36) [orig: the catalog rows 90 @0x817FC3 and 91 @0x81802F;
// Input_HandleActionBinding @0x49AD40 - cases 104 / 105 @0x49BAB3..0x49BB12].
enum ChatTalkRow : int {
	kChatRowTalk,
	kChatRowLocal,
	kChatRowGlobal,
	kChatRowTeam,
	kChatRowSquad,
	kChatRowCrew,
	kChatRowCommand,
	kChatRowCommand2,
	kChatRowRed,
	kChatRowBlue,
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
	// One input frame's talk-row poll over the sampled rows (a mask of 1 <<
	// ChatTalkRow). The talk rows are action bindings the input frame's key
	// scan fires and its drain dispatches every tick, after the ring handed
	// any typed keys to an open line, so the embedder polls in every input
	// frame (each banked tick's) and a one-tick press opens its line on its
	// tick; the command arm reads no local player (a non-peer authority's
	// dispatch takes its authority word as 2 without one, and the command
	// row's flags 0x5000800 carry none of the dispatch gates), so a Serve Only
	// host opens the prompt too: the dispatch first offers each action to the
	// hook at +0x170 of g_LocalPlayerEntity's def (+0x20), a nonzero return
	// consuming it, and every role's mission start points that word at its own
	// row body (fatal with none), whose def's hook is its input name's table
	// entry: troop and tank take Input_HandleActionBinding_0, whose switch
	// sends code 1 to its default return of 0, and the null entry's hook
	// returns 0, so no hook takes the command row [orig: Game_StartMission
	// @0x524360 (the Player_InitPlayer call @0x525BBC); Player_InitPlayer
	// @0x4E15F0 (the Player_FindLocalPlayerEntity store @0x4E17EB, the fatal
	// @0x4E17F2); EntityDef_InitAllCallbacks @0x4A5A70 (the def's +0x168 name,
	// "Null" when empty, @0x4A5B50..0x4A5B67); EntityDef_LookupInputCallbacks
	// @0x497910 (the table @0x829DA8, the +0x170 stores @0x497949 / @0x497965);
	// Input_HandleActionBindingNull @0x497980; Input_HandleActionBinding_0
	// @0x4E0420 (the range test @0x4E0475..0x4E047E, the default @0x4E13F9);
	// Input_ProcessFrame @0x49D520
	// (the drain's Input_HandleActionBinding call @0x49D5D8);
	// Input_HandleActionBinding @0x49AD40 (the authority word
	// @0x49AD52..0x49AD74; the row gates @0x49AD8A..0x49AE23: flag 0x1 with an
	// authority word, with dword_A87050 or under g_RoundOver, 0x40 without an
	// authority word, 0x400 with the pre-round timer, 0x8000000 with the
	// deploy screen, 0x10 in a session; the def hook @0x49AE94..0x49AEBD; the
	// command arm, cases 1 and 20, @0x49AEF7..0x49AF10); the command row's
	// code 1 and flags @0x81779C / @0x8177A0]. Each row's press latch steps on the ungated key state; a
	// press edge dispatches its row while gameplay input is active, no chat
	// capture is open and the editor took no key this frame — a key the
	// editor consumed (the Enter that submitted) never also re-dispatches,
	// as retail's router hands each key to exactly one of the two
	// [orig: Input_ProcessKeyboardEvents @0x49d2e3 — the capture branch
	// @0x49d498 vs the special-keys/binding scan @0x49d2fb..0x49d488].
	uint32_t poll_rows(uint32_t rows_down, bool active,
			const ChatEntryFacts &facts, const GameTextLookup &gametext);
	// One input frame's dispatched talk-row events (D-CTRL-6): each row set in
	// `rows_fired` had one queued event this frame (a key-down the keyboard pass
	// scanned to it, an OS repeat included, or a press of its mouse or joystick
	// arm), dispatched in row order while gameplay input is active and no capture is
	// open; once a row opened the capture the later rows' events find it open. A key
	// the open line took never reached the scan (the capture branch), so no edge
	// latch or editor-consumed test stands between.
	// [orig: Input_ProcessKeyboardEvents @0x49d1f0 - the capture branch @0x49d2e3 ahead
	//  of the scan's fires @0x49d42f / @0x49d488; Input_ProcessFrame @0x49d520 - the
	//  drain's Input_HandleActionBinding call @0x49d5d8]
	uint32_t dispatch_rows(uint32_t rows_fired, bool active,
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
