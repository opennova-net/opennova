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

// Where the join stands, as the join machine's status line reads it.
enum class JoinScreenStage : int {
	Joining = 0,     // the dial: ServerHello / ServerAuth not yet accepted (join state 4)
	Connecting = 1,  // accepted; awaiting the host's S2C 0x00 (state 5)
	Verifying = 2,   // S2C 0x00 answered; the S2C 0x01 verification is not 1 yet (state 6)
	Queued = 3,      // verified; waiting in the host's join queue for S2C 0x05 (state 6)
	Starting = 4,    // S2C 0x05: the game starts and the loading screen takes over (state 7)
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

// The status line for a stage: gameerr "Generic Strings" PRE_JOININGSESSION /
// PRE_CONNECTING / PRE_VERIFYING / PRE_CONNECTED, and for Queued the queue
// line: WAITXXXPEOPLEINFRONT1 (position - 1 ahead) / 2 (one ahead) / 3 (next)
// followed by a space and WAITTIMEDISPLAY (hours, minutes, seconds queued),
// or WAITTOJOIN when the record holds no valid position.
// [orig: MultiPlayer_JoinSessionStateMachine — "Joining Session..." stamped
//  before the dial (state 4), PRE_CONNECTING on its success, PRE_VERIFYING
//  @0x56a649, the queue line @0x56a75e..0x56a8f1, PRE_CONNECTED @0x56a90b]
std::string join_screen_text(JoinScreenStage stage, const JoinQueueRecord &queue,
		uint64_t now_ms, const rtxt::File *override_table, const rtxt::File *gameerr);

} // namespace opennova::inmatch
