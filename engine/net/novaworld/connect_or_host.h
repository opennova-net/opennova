#pragma once

// The NovaWorld menu's hosting leg ahead of its request: the gates CNapiGameSession_ConnectOrHost
// runs on the logged-in game session before ClientHostRequest ships, and the player cap it
// clamps. The request, the 60 s poll and the result map ride ClientSession and the NWEC tables
// (net/napi/session.h).

#include <base/gameprofile/gameprofile.h>
#include <net/napi/session.h> // the NWEC tags

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova {

// "NWU NOT SET UP" keeps the leg's preset tag [orig: CNapiGameSession_ConnectOrHost @0x4d4f10 —
// the preset @0x4d4f89, the bail @0x4d4f95..0x4d4fd2].
inline constexpr char NWEC_SESSION_NOT_SET_UP[] = "NWEC01";

// The menu's Host runs none of these checks: it opens the main page's @HOST_URL@ (http_flow.h's
// hosting line), and the hosting page whose title carries the HOSTKEY dispatches the leg; only then
// do the checks and their lines run, over the session and the gate reply as they then stand.
// [orig: UI_HandleHostSessionStart @0x556D00 - the open @0x556F98, no check ahead of it;
//  UI_DispatchScreenEvent @0x54E6A0 - HostDialog_StartSession @0x54F321,
//  CNapiGameSession_ConnectOrHost (the call @0x54F33B), the error dialog @0x54F349]
//
// The tag the hosting leg fails with before its request, or nullptr to send it: a session that
// is not set up (NWEC01), one outside states 4..8 -- flags bits 2 and 8 -- (NWEC49), and a gate
// reply missing or without a lobby name (NWEC50).
// [orig: CNapiGameSession_ConnectOrHost @0x4d4f10 — the flags test @0x4d4fd8..0x4d56f6, the
//  gate test `dword_B5FF48 && g_LobbyName[0]` @0x4d5085..0x4d50dc]
inline const char *host_leg_refusal(bool session_set_up, uint32_t session_flags, bool gate_ok,
		const std::string &lobby_name) {
	if (!session_set_up) return NWEC_SESSION_NOT_SET_UP;
	if ((session_flags & 0x2u) == 0 || (session_flags & 0x8u) == 0) return NWEC_WRONG_SESSION_STATE;
	if (!gate_ok || lobby_name.empty()) return NWEC_HOST_GATE_NOT_OK;
	return nullptr;
}

// The leg's MaxPlayers clamp: at least 1, at most 64, or 65 for a dedicated host; dfx2.exe's
// (`title` a gameprofile::GameId) at most 250, or 251 dedicated (docs/titles/dfx2-vs-jo-re.md
// DFX2-8).
// [orig: CNapiGameSession_ConnectOrHost @0x4d504e..0x4d507b] [orig: CNapiGameSession_ConnectOrHost
//  @0x4d523b..0x4d526f (dfx2.exe)]
inline int host_leg_max_players(int max_players, bool dedicated, int title) {
	if (title == gameprofile::GAME_DFX2) return std::clamp(max_players, 1, dedicated ? 251 : 250);
	return std::clamp(max_players, 1, dedicated ? 65 : 64);
}

// The menu's play and host legs' _connectlog.txt lines, past their "<time> : " prefix (the
// device stamps them). The command-line host runs neither leg and writes none of them.
// [orig: CNapiGameSession_ConnectOrHost @0x4D4F10 - each line's NapiFile_AppendLine at the
//  address beside it]
inline constexpr char kConnectOrHostStartLine[] = "NWU START HOSTING OR PLAYING.";   // @0x4D4F60
inline constexpr char kConnectOrHostNotSetUpLine[] = "NWU NOT SET UP!!!";            // @0x4D4FD2
inline std::string connect_or_host_wrong_state_line(int session_state) {             // @0x4D56E3
	return "NWU NOT CORRECT STATE[" + std::to_string(session_state) + "]!!!";
}
inline constexpr char kPlayAttemptLine[] = "NWU ATTEMPTING TO START PLAYING";        // @0x4D53AE
inline constexpr char kPlayStartFailedLine[] = "NWU STARTPLAYING failed!";           // @0x4D5486
inline constexpr char kPlayEscapedLine[] = "NWU STARTPLAYING user pressed escape!!!"; // @0x4D5577
inline constexpr char kPlayTimeoutLine[] = "NWU STARTPLAYING timout!!!";             // @0x4D552C
inline std::string play_not_playing_line(int session_state) {                         // @0x4D55E7
	return "NWU STARTPLAYING failed.  Not in playing state[" + std::to_string(session_state) +
	       "]!!!";
}
inline constexpr char kMovingOnLine[] = "NWU MOVING ON TO HOSTING OR PLAYING...";    // @0x4D536D
// The play leg's 60 s poll (the game shell's NovaWorldClient runs it): the key queue emptied
// before it (Input_ResetKeyQueue @0x4D54A1); each frame the periodic update, the timeout test, then
// the key read (Input_DequeueKeyEvent @0x4D54DA): ESC writes kPlayEscapedLine with NWEC03
// (@0x4D558A), a timeout kPlayTimeoutLine with NWEC02 (@0x4D553E), and either runs
// CGameSession_StopPlaying (the call @0x4D5592); then any state but 8 writes
// play_not_playing_line (@0x4D55E7) and maps a result's MsgCode over the tag (@0x4D55FE).
// The legs' two waits are GetTickCount loops inside the menu frame that runs ConnectOrHost, each
// until its result or 60 s, so that frame (and its remote-admin pump) ends only with the wait: the
// game shell's NovaWorldClient feeds the wait into inmatch::RemoteAdminIdleFrame. The host leg's
// loop pumps the session's periodic update alone, reading no window message and no key, so the
// player's ESC is never taken while the host request waits; the play leg's adds the window
// messages and the key read (the play poll above). The gate wait and the connect polls run frame
// by frame. [orig: CNapiGameSession_ConnectOrHost @0x4D4F10 - the host loop @0x4D5180..0x4D519D
//  (ProcessPeriodicUpdate @0x4D5185, the 0xEA60 compare @0x4D518E, the state test
//  @0x4D519A..0x4D519D), the play loop @0x4D54C0..0x4D54EE (ProcessPeriodicUpdate @0x4D54C5,
//  Game_PumpWindowMessages @0x4D54D5, Input_DequeueKeyEvent @0x4D54DA); its one caller
//  UI_DispatchScreenEvent (the call @0x54F33B)]
// The play poll's exit: a pass whose timeout or key read stopped the play (StopPlaying), or one
// that finds the session out of state 7, leaves the loop; state 8 is the result's success
// (kMovingOnLine), any other writes play_not_playing_line and shows the leg's tag: the preset
// NWEC01, the timeout's NWEC02 or the escape's NWEC03, replaced by the MsgCode word's map when
// the word is nonzero (a refusal, a ServerStopPlaying, a punt; the play request cleared it, so
// the reap and a reconnect's state leave the tag).
// [orig: CNapiGameSession_ConnectOrHost @0x4D4F10 - the preset @0x4D4F89, the loop's state test
//  @0x4D54E7..0x4D54EE, StopPlaying @0x4D5592, `cmp dword_B600FC,8` @0x4D559C, @0x4D55E7, the
//  word's zero test @0x4D55F7..0x4D55FE, the map @0x4D5604..0x4D569F]
inline std::string play_leg_exit_tag(const std::string &tag, int msg_code_word) {
	return msg_code_word != 0 ? novaworld_play_error_tag(msg_code_word) : tag;
}
inline constexpr char kHostAttemptLine[] = "NWU ATTEMPTING TO START HOSTING";        // @0x4D5038
inline constexpr char kHostGateNotOkLine[] = "NWU HOSTING FAILED...GATEINFO NOT OK!!!"; // @0x4D50C9
inline constexpr char kHostStartFailedLine[] =
		"NWU HOSTING FAILED...StartHostingSession failed!!!";                         // @0x4D5153
inline constexpr char kHostNotHostingLine[] =
		"NWU HOSTING FAILED...WE'RE NOT IN HOSTING STATE!!!";                         // @0x4D51DF
inline constexpr char kHostStartedLine[] = "NWU HOSTING STARTED!  YIPPEE";           // @0x4D532D
inline constexpr char kHostTimeoutWaitingLine[] =
		"NWU HOSTING FAILED...TIMEOUT WAITING FOR REPLY FROM NOVAWORLD!!!";           // @0x4D5245
inline constexpr char kHostTimeoutLine[] = "NWU HOSTING TIMEOUT!!!";                 // @0x4D528E

// The legs' checks ahead of their work, in ConnectOrHost's order, as lines: the start line,
// then a session not set up, else one outside the right states (its state), else the leg's
// attempt line and, for the host leg, a gate reply missing or without a lobby name. `refusal`
// is the leg's tag (host_leg_refusal's, or the play leg's same first two), nullptr to go on.
// [orig: CNapiGameSession_ConnectOrHost @0x4D4F10 - @0x4D4F60, the not-set-up test
//  @0x4D4F95..0x4D4FD2, the flags test @0x4D4FD8 -> @0x4D56E3, the play leg @0x4D53AE, the host
//  leg @0x4D5038, the gate test @0x4D5085..0x4D50C9]
inline std::vector<std::string> connect_or_host_check_lines(bool host_leg, const char *refusal,
		int session_state) {
	std::vector<std::string> lines{ kConnectOrHostStartLine };
	const std::string tag = refusal != nullptr ? refusal : "";
	if (tag == NWEC_SESSION_NOT_SET_UP) {
		lines.push_back(kConnectOrHostNotSetUpLine);
		return lines;
	}
	if (tag == NWEC_WRONG_SESSION_STATE) {
		lines.push_back(connect_or_host_wrong_state_line(session_state));
		return lines;
	}
	lines.push_back(host_leg ? kHostAttemptLine : kPlayAttemptLine);
	if (host_leg && tag == NWEC_HOST_GATE_NOT_OK) lines.push_back(kHostGateNotOkLine);
	return lines;
}

// The play leg's checks: a session not set up (NWEC01), one outside the verified states -- flags
// bits 2 and 8 -- (NWEC49); nullptr to go on.
// [orig: CNapiGameSession_ConnectOrHost @0x4D4F10 - @0x4D4F89..0x4D4FD2, @0x4D4FD8..0x4D56F6]
inline const char *play_leg_refusal(bool session_set_up, uint32_t session_flags) {
	if (!session_set_up) return NWEC_SESSION_NOT_SET_UP;
	if ((session_flags & 0x2u) == 0 || (session_flags & 0x8u) == 0) return NWEC_WRONG_SESSION_STATE;
	return nullptr;
}

} // namespace opennova
