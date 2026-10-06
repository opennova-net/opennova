#pragma once

// THE SERVER'S CONSOLE: what a host keeps and says beside its players, and
// the session log lines its protocol writes.
//
// The CHAT ring of a host with no client of its own (Serve Only,
// opennova-serve). Retail's chat dispatcher posts into it on that host only:
// every player line the chat handler fans (after the fan, by the channel the
// handler resolves) and the host's own Global line, each unwrapped and cut at
// 119 characters into the newest of 40 raw slots. The status page shows its
// four newest slots [orig: NapiNPServer_HandleChatMessage — the
// `!is_mp_session_peer` posts @0x514147..0x51415b and their siblings in each
// channel arm; Chat_SendTeamMessage @0x49aa2a; Chat_DispatchToChannel
// @0x42b910 -> Chat_AddMessageChannel1 @0x4985d0, the raw shift
// @0x4985d7..0x4985f4 and the 119-byte copy @0x49861e..0x498654;
// HUD_DrawServerConsoleLines @0x5ba0bd reads the CHAT raw slots 3..0 on a
// non-peer]. A listen host's page reads its client's SYSTEM ring instead
// (the HUD presenter's), so nothing here runs for it.
//
// The console chat. A host with no client sends chat through one sender
// only: the Global talk's authority arm, which fans S2C 0x14 [10][255][text]
// to every in-match slot and posts the line to the host's own CHAT ring in
// white. Every other talk row queues a C2S 0x0D on the local client
// connection, which such a host never creates, so it sends nothing
// [orig: Chat_SendTeamMessage (IDB misnomer — the GLOBAL sender) @0x49a900 —
// `is_mp_session_peer` @0x49a980, the authority arm @0x49a9da..0x49aa2a;
// CNapiNetwork_QueueReliableMessage @0x4c4fa0 no-ops without ctx+0xE60, which
// CNapiGameSession_CreateSession @0x4c9b6a..0x4c9b9c creates only for an
// authority that is also a session peer].
//
// The /INOUT lines: the protocol's HOST STARTED / HOST STOPPED and its
// per-connection PLAYER ADDED / PLAYER REMOVED, which retail writes to the
// protocol's `_inout.txt` log when `/INOUT` armed it; the port emits them on
// the diagnostic channel (io::logf, kInfo) at the same points (D-NET-356)
// [orig: CNapiNPConnection_LogHostStarted @0x61e6a0 (StartServer @0x62b629),
// CNapiNPConnection_LogHostStopped @0x61e780 (StopServer @0x62a97b),
// CNapiNPConnection_LogPlayerAdded @0x61e860 (OnStateChange @0x626204),
// CNapiNPConnection_LogPlayerRemoved @0x61e940 (TeardownActiveConnection
// @0x6253d2); the log CNapiLog_Init(protocol + 0xF80, "_inout.txt")
// @0x625afb, enabled by NapiLog_SetEnabled(dword_B4C688) @0x4c6d15 after
// the socket opens; `/INOUT` @0x4a743d..0x4a744f].

#include <runtime/hud/game_text_lookup.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::hud {
enum class ChatSendResult : uint8_t;
class ChatEntry;
} // namespace opennova::hud

namespace opennova::inmatch {

struct NapiNPServerCtx;
struct NapiNPConnection;

struct ServerConsoleLine {
	std::string text;
	uint32_t color = 0xFFFFFFFFu; // the packed ARGB the post stored
};
inline constexpr size_t kServerConsoleRingSlots = 40;  // [orig: byte_B3EA38, 40 x 128]
inline constexpr size_t kServerConsoleLineMax = 119;    // [orig: `cmp eax, 78h` @0x49861e]
inline constexpr size_t kServerConsoleShownLines = 4;   // [orig: raw slots 3..0 @0x5ba0bd]

// Chat_AddMessageChannel1's raw leg: shift the ring and store the line (cut at
// 119) with its colour in the newest slot. `ring` holds the newest slot last.
void server_console_post(std::vector<ServerConsoleLine> &ring, const std::string &text,
		uint32_t argb);

// Chat_DispatchToChannel on a host with no client: the channel picks the sink
// and colour (hud/feed_format.h chat_channel_sink / chat_channel_color); a
// CHAT-sink line lands in ctx's ring, the SYSTEM ring and the rest are not the
// page's. The dispatcher's sender gate never drops a line here: the mute bit it
// tests is a client's, and the handler admits a spectator only while the spawn
// gate is up [orig: @0x42b91e..0x42b943].
void server_console_dispatch(NapiNPServerCtx &ctx, int channel, const std::string &text);

// The status page's four console rows, oldest at the top; a slot never posted
// is an empty row [orig: HUD_DrawServerConsoleLines @0x5ba0a0, the zero
// colour rewritten to -1 @0x5ba0de].
std::vector<ServerConsoleLine> server_console_rows(const std::vector<ServerConsoleLine> &ring);

// The talk sender on a host with no client: Global (dispatch 101) runs the
// authority arm — the gates `(!death screen || spawn gate)` and a non-empty
// line, the flood table (a refusal is Flooded: the caller echoes the line in
// the dispatch's flood colour), the tag strip, S2C 0x14 {channel 10, sender
// 0xFF, the stripped line} reliable with the 310-flush lifetime to every
// in-match slot (send mask 0x80), then the unstripped line to the host's CHAT
// ring in white — and returns Broadcast. Every other dispatch is Refused (its
// sender's queue has no connection). `frame` is the per-main-frame counter.
// [orig: Chat_SendTeamMessage @0x49a900 — the gates @0x49a931, flood
//  @0x49a93b, strip @0x49a971, mask 128 @0x49a9f2, NetPacket_WriteTwoBytesAndCString
//  (.., 255, 10, ..) @0x49aa18, SendFiltered(0x14, 1, 310) @0x49aa1d,
//  Chat_DispatchToChannel(0xFF, 10, message) @0x49aa2a]
hud::ChatSendResult Server_SendConsoleChat(NapiNPServerCtx &ctx, int dispatch,
		std::string &text, uint32_t frame);

// One line typed into a client-less host's chat input, as its keyboard would
// type it: the Global talk row opens the capture (its gates and debounce),
// each byte goes through the line editor's own rules (a printable byte while
// under the 60-character cap; a backslash cycles the talk channel), and Enter
// sends through Server_SendConsoleChat; a flood refusal echoes the line into
// the ring in Global's flood colour. Returns the editor's events. The talk row
// reaches such a host: the dispatcher reads its binding flags by action code
// after the start-up re-lay, and record 101 (flags 0x05000800) carries none of
// the head gate's bits, so the gate that drops a flag-0x1 action on a Serve
// Only host passes it, as the keyboard scan's own copy of that test does.
// [orig: Input_HandleActionBinding @0x49AD8D..0x49AE23 (the head gate),
//  KeyBinding_SortBySequentialId @0x498260, Input_ProcessKeyboardEvents
//  @0x49d330..0x49d42f; case 101 @0x49b989..0x49b9f4 -> Chat_BeginChatInput
//  @0x498060; Chat_HandleInputChar @0x49cb70 — Enter @0x49cd87..0x49ce6a; the
//  flood echo Chat_AddMessageChannel1(message, palette[3], 930) @0x49a953]
uint32_t server_console_submit(NapiNPServerCtx &ctx, hud::ChatEntry &entry,
		const std::string &line, uint32_t frame, const hud::GameTextLookup &gametext);

// The /INOUT lines, with Napi_FormatAddress's "a.b.c.d:port" (retail's
// "???.???.???.???:?????" when the host has no socket).
std::string inout_host_line(const NapiNPServerCtx &ctx, bool started);
std::string inout_player_added_line(const NapiNPConnection &conn);
std::string inout_player_removed_line(const NapiNPConnection &conn);

} // namespace opennova::inmatch
