#pragma once

// The join screen: retail joins a session in its own game mode ("PreMenu", menu
// file pre.mnu, screen PRE_GAME_MENU), not under the loading screen. A join
// machine runs the dial, the connect, the verification and the queue, stamping
// one status line at a time into the MESSAGES box; a panel machine picks which
// of the screen's windows show. A failure writes the connection's reason text
// (disconnect_reason.h) into MESSAGES and parks the screen with only Cancel,
// and the loading screen starts only once the host signals the game start.
// [orig: MultiPlayer_JoinSessionStateMachine @0x56a320 (the join machine,
//  dword_25E58A0); UI_ShowPreGameMenuByState @0x568d10 (the panel machine's
//  windows, dword_25E5880); the PreMenu init sub_5693E0]

#include <cstdint>
#include <string>

#include <formats/rtxt/rtxt.h>

namespace opennova::inmatch {

// pre.mnu's windows the panel machine shows and hides, as bits.
enum PreGameWindow : uint32_t {
	kPreGameErrorWrapper = 1u << 0,            // ERROR_WRAPPER (the MESSAGES box)
	kPreGameAbortWrapper = 1u << 1,            // ABORT_WRAPPER (ABORT_CANCEL)
	kPreGameAbortRetryWrapper = 1u << 2,       // ABORTRETRY_WRAPPER (BACK / ACCEPT)
	kPreGameGamePasswordWrapper = 1u << 3,     // GAME_PASSWORD_WRAPPER
	kPreGameSpectateWrapper = 1u << 4,         // SPECTATE_WRAPPER
	kPreGameTeamPasswordWrapper = 1u << 5,     // TEAM_PASSWORD_WRAPPER
	kPreGameTeamStaticWrapper = 1u << 6,       // TEAM_STATIC_WRAPPER (inside TEAM_PASSWORD)
	kPreGameSpectatorStaticWrapper = 1u << 7,  // SPECTATOR_STATIC_WRAPPER (inside TEAM_PASSWORD)
};
inline constexpr int kPreGameWindowCount = 8;

// The window name each bit shows, in bit order.
const char *pre_game_window_name(int bit_index);

// The panel machine's states [orig: dword_25E5880].
enum class PreGamePanel : int {
	Error = -1,            // a failure: the reason in MESSAGES, Cancel only
	Progress = 0,          // the join's status line, Cancel
	GamePassword = 1,
	Spectate = 2,
	SpectatorPassword = 3,
	TeamPassword = 4,
};

// The windows a panel state shows; every other window hides.
// [orig: UI_ShowPreGameMenuByState @0x568d10]
uint32_t pre_game_windows(PreGamePanel panel);

// The choice: the join machine's state 8 (PRE_JOINSESSIONCHOICE), which a discovered
// session's flag word reaches when it carries any of 0x6038 (server_flags.h
// join_entry_step). The progress panel's arm then opens the first panel the word asks
// for: GAME_PASSWORD for a server password (0x8), else SPECTATE when it takes
// spectators (0x2000), else TEAM_PASSWORD for a side password (0x30) on a process that
// is no authority; else no panel (Progress).
// [orig: UI_ShowPreGameMenuByState @0x568d10 - the arm @0x568dfc..0x568e44]
PreGamePanel pre_game_entry_panel(uint32_t server_flags, bool authority);

// ACCEPT on a panel (the ACCEPT control's activation, at the choice state): the next
// panel the word asks for, or the commit (pre_game_commit), which returns the screen to
// Progress and the join machine to the state it left. GAME_PASSWORD goes on to SPECTATE
// (0x2000), else TEAM_PASSWORD (0x30, no authority), else commits; SPECTATE checked goes
// on to the spectator password (0x4000, no authority), unchecked to TEAM_PASSWORD (0x30,
// no authority), else commits; either password panel commits. ACCEPT on Progress does
// nothing; on Error it aborts the join (as BACK and ABORT_CANCEL always do,
// PreMenu_OnAbortCancel).
// [orig: UI_PreGameMenuStateMachine @0x568A40 - case 1 @0x568a81..0x568ac7, case 2
//  @0x568aed..0x568b4b (CCheckWnd_GetCheckedState on SPECTATE @0x568b0e), cases 3 and 4,
//  the default (abort: g_PreMenuAbortRequested @0x568b57, PreMenu state 9 @0x568b61), the
//  commit @0x568b7e; PreMenu_OnAbortCancel @0x568CA0 registered on BACK and ABORT_CANCEL
//  (PreMenu_Init @0x569487 / @0x5694a5)]
struct PreGameAccept {
	PreGamePanel next = PreGamePanel::Progress;
	bool commit = false;
	bool abort = false;
};
PreGameAccept pre_game_accept(PreGamePanel panel, uint32_t server_flags, bool spectate_checked,
		bool authority);

// What the commit copies out of the screen's three controls into the session settings:
// GAME_PASSWORD's text is the server password; SPECTATE's state the spectate choice
// (JSR); TEAM_PASSWORD's one shared text goes to the spectator password (JSPP) when
// SPECTATE is checked, else to the side password (JSP), the other keeping what it held;
// the side requirement is automatic (-1).
// [orig: UI_PreGameMenuStateMachine @0x568b7e..0x568c69 - joinPassword_361 @0x568bc3,
//  unknownHostOption_428 @0x568bf0, unknownHostText_42C @0x568c33 / sidePassword_372
//  @0x568c45, sideRequirement_424 and g_SideRequirement = -1 @0x568c55..0x568c5a]
struct PreGameChoice {
	std::string server_password;
	bool spectate = false;
	std::string spectator_password;
	std::string side_password;
};
void pre_game_commit(const std::string &game_password, bool spectate_checked,
		const std::string &team_password, PreGameChoice &choice);

// Where the join stands, as the join machine's status line reads it.
enum class JoinScreenStage : int {
	Joining = 0,     // the dial: ServerHello / ServerAuth not yet accepted (join state 4)
	Connecting = 1,  // accepted; awaiting the host's S2C 0x00 (state 5)
	Verifying = 2,   // S2C 0x00 answered; the S2C 0x01 verification is not 1 yet (state 6)
	Queued = 3,      // verified; waiting in the host's join queue for S2C 0x05 (state 6)
	Starting = 4,    // S2C 0x05: the game starts and the loading screen takes over (state 7)
	Enumerating = 5, // the enumerate-and-join's browse, until a session answers (state 3)
};

// The host's S2C 0x03 join-queue record as the client keeps it: queued, the
// player's position and the queue's length, and when this queued run began.
// [orig: NapiNPClientMsg_0x003 @0x425390 — +0x2F4 queued, +0x2F8 position,
//  +0x2FC length, +0x300 the GetTickCount stamp of the first queued record]
struct JoinQueueRecord {
	bool queued = false;
	int32_t position = 0;
	int32_t length = 0;
	uint64_t queued_since_ms = 0;
};

// Fold one S2C 0x03 body: [u8 queued][u16 position][u16 length], each field
// read as 0 past the end; position and length are 0 when not queued, and the
// queued-since stamp is taken only on a not-queued -> queued edge.
void fold_join_queue_record(JoinQueueRecord &record, const uint8_t *data, size_t size,
		uint64_t now_ms);

// The status line for a stage: gameerr "Generic Strings" PRE_ENUMSESSION /
// PRE_JOININGSESSION / PRE_CONNECTING / PRE_VERIFYING / PRE_CONNECTED, and for Queued the queue
// line: WAITXXXPEOPLEINFRONT1 (position - 1 ahead) / 2 (one ahead) / 3 (next)
// followed by a space and WAITTIMEDISPLAY (hours, minutes, seconds queued),
// or WAITTOJOIN when the record holds no valid position.
// [orig: UI_EnumerateAndJoinSession @0x569f50 - PRE_ENUMSESSION on the pre-game
//  menu @0x56a0ac..0x56a0b4 ahead of the enumerate loop;
//  MultiPlayer_JoinSessionStateMachine — "Joining Session..." stamped
//  before the dial (state 4), PRE_CONNECTING on its success, PRE_VERIFYING
//  @0x56a649, the queue line @0x56a75e..0x56a8f1, PRE_CONNECTED @0x56a90b]
std::string join_screen_text(JoinScreenStage stage, const JoinQueueRecord &queue,
		uint64_t now_ms, const rtxt::File *override_table, const rtxt::File *gameerr);

} // namespace opennova::inmatch
