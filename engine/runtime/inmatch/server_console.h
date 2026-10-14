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

#include <formats/fnt/fnt.h>
#include <net/npwire/protocol_message.h>
#include <runtime/hud/game_text_lookup.h>
#include <runtime/inmatch/server_files.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

namespace opennova::hud {
enum class ChatSendResult : uint8_t;
class ChatEntry;
} // namespace opennova::hud

namespace opennova::def {
struct DefItemsFile;
}

namespace opennova::world {
class World;
}

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

// THE CHAT DISPLAY BUFFER: Chat_AddMessageChannel1's display leg, the 40 slots
// of 128 bytes CHAT GET lists. Each post word-wraps the whole line with the
// bold label font at its native size against the chat box's width, the
// hud.def `chat_message` row's x2 - (x1 - 4); a host with no hud.def (JO:CA
// ships none) keeps both rows 0, so the width is 4, and a line splits only
// where the walk meets a space before its first glyph's advance passes 4: a
// leading space, or one narrow glyph and a space ("i am" in Arial16b). The
// buffer shifts up by the segment count, the new slots are zeroed, slot 1's
// timer takes the post's life (930) or slot 2's plus 186 when that is later,
// and the segments fill slots count..1: the first as it stands, each later
// one behind two spaces, each copy at most 119 bytes (the cursor advancing by
// the copy, so a longer segment's tail reads as the next one), then the
// colour at +120. A slot is its text bytes (+0..+119), its colour dword
// (+120) and its timer dword (+124); slot 1 is the newest.
// [orig: Chat_AddMessageChannel1 @0x4985D0 — the width from HUD_GetChatBoxCoord
//  @0x5BBE90 rows 2 and 1 @0x498673..0x49869C (g_HUDChatBoxCoords @0x28E4DF8,
//  written only by the hud.def parser @0x5BB7D1 / @0x5BB7ED), HUD_WordWrapText
//  @0x580980 over g_hudLabelFontBold @0x4986AC, a 0 count read as 1
//  @0x4986BB..0x4986C5, the shift @0x4986CC..0x498701, the zeroing @0x498718,
//  slot 1's timer @0x49872C..0x498734, the fill @0x498750..0x4987DA; the lives
//  930 at every Chat_DispatchToChannel call @0x42B988..0x42BAEA]
inline constexpr size_t kChatDisplaySlotCount = 40;
inline constexpr size_t kChatDisplaySlotBytes = 128;
inline constexpr size_t kChatDisplayColorOffset = 120;
inline constexpr size_t kChatDisplayTimerOffset = 124;
inline constexpr size_t kChatDisplayCopyMax = 119;
inline constexpr int32_t kChatDisplayLife = 930;     // [orig: `push 3A2h`]
inline constexpr int32_t kChatDisplayStagger = 186;  // [orig: dword_B3FF38 + 186 @0x498722]
// The chat box with no hud.def: both rows 0, 0 - (0 - 4).
inline constexpr int kChatDisplayWidthNoHudDef = 4;

struct ChatDisplayBuffer {
	// Slot k (1..40) at byte (k - 1) * 128, slot 40 the oldest.
	std::array<uint8_t, kChatDisplaySlotCount * kChatDisplaySlotBytes> bytes{};
};
// The wrap's font (g_hudLabelFontBold: the bold Arial HUD_InitAllFonts picks by
// the display's width; null, a font that did not load: no line wraps) and the
// chat box's width.
struct ChatDisplayWrap {
	const fnt::fnt_font_t *font = nullptr;
	int width = kChatDisplayWidthNoHudDef;
};

// One line into the display buffer, with its colour and life. A wrap of more
// than 40 segments writes only the 40 slots (retail's runs past the buffer,
// D-NET-711).
void chat_display_post(ChatDisplayBuffer &buffer, const ChatDisplayWrap &wrap,
		const std::string &text, uint32_t argb, int32_t life = kChatDisplayLife);
// One logic tick: every slot's nonzero timer steps down by one
// [orig: Input_DecrementCooldownTimers @0x498440 (the dword_B3FEB8 column)
//  from Game_ProcessMainFrame @0x5267A6].
void chat_display_tick(ChatDisplayBuffer &buffer);
// CHAT GET's text: slots 40..1, each slot's bytes read as a string, the
// non-empty ones each followed by CR LF. A slot whose 120 text bytes hold no
// NUL reads on into its colour and timer words and the next slot's bytes, as
// retail's strlen does; the buffer's end stops it here.
// [orig: CAdminServer_HandleChatCommand @0x404B11..0x404BA5]
std::string chat_display_get(const ChatDisplayBuffer &buffer);

// Chat_AddMessageChannel1 on a host with no client: the raw ring's line and
// the display buffer's, over the context's wrap (the dispatcher's 930 life).
void server_console_add(NapiNPServerCtx &ctx, const std::string &text, uint32_t argb);

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

// THE NET LOG: the protocol manager's NapiLog, which the process creates with
// its network (buffered, mode 3; a line stamp; a 5000 ms flush interval) and
// which only NETLOG, the `/NETLOG` switch and a `_DONETLOG.TXT` in the working
// directory turn on, naming its file `_NETLOG.TXT`. Nothing in the image writes
// a line into it but its own start and stop lines: an enable appends
// "NapiLog Started!", a disable "NapiLog Stopped!", each behind the stamp
// "yyyy-mm-dd hh:mm:ss: " and followed by CR LF, into the log's buffer. The
// manager's pump flushes the buffer once 5000 ms have passed since the last
// flush; the log's first open of the file truncates it, every later one
// appends, and each flush closes it again; the shutdown stops the log (its
// stop line when it is on) and flushes.
// [orig: CNapiNetwork_SetNetLogFile @0x4C69A0 (NapiLog_SetFilename "_NETLOG.TXT",
//  NapiLog_SetEnabled); NapiNPManager_Init @0x62C4BB..0x62C4D0 (CNapiLog_Init
//  "netlog.txt", mode 3, flags 6: the stamp 4 and the append-once 2, 5000);
//  NapiLog_SetEnabled @0x62F2B0; NapiLog_WriteEntry @0x62EE30 (the stamp
//  "%4.4ld-%2.2ld-%2.2ld %2.2ld:%2.2ld:%2.2ld: " of Napi_GetLocalTime @0x61D160,
//  NapiBuffer_AppendLine @0x634BB0 and its "\r\n" @0x7C1310);
//  NapiLog_FlushIfExpired @0x62F2F0 from NapiNPManager_Pump @0x62AC98;
//  NapiLog_Flush @0x62F1A0; NapiLog_EnsureOpen @0x62EDC0 (NapiFile_Open 0x6A, its
//  0x20 the truncate, the first time, else 0x4A); NapiLog_Shutdown @0x62F310 from
//  NapiNPManager_Shutdown @0x62B739; the switch @0x4A7A11..0x4A7A29 and
//  _DONETLOG.TXT @0x4A7CBC..0x4A7CD3 in Game_ParseCommandLineAndInit]
inline constexpr const char *kNetLogFileName = "_NETLOG.TXT";
inline constexpr const char *kNetLogArmFileName = "_DONETLOG.TXT";
inline constexpr uint32_t kNetLogFlushIntervalMs = 5000;

// NapiLog_WriteEntry's stamp over local time `t`: "yyyy-mm-dd hh:mm:ss: ".
std::string napi_log_stamp(std::time_t t);

class NetLog {
public:
	explicit NetLog(ServerFileSink &files);

	// The stamp text the entries take (napi_log_stamp over the current time by
	// default); a test pins it.
	void set_stamp_text(std::function<std::string()> stamp) { stamp_ = std::move(stamp); }

	bool enabled() const { return enabled_; }
	// NapiLog_SetEnabled: a change of state writes its line; the same state none.
	void set_enabled(bool on);
	// NapiLog_FlushIfExpired at the manager's pump, `now_ms` the process clock.
	void flush_if_expired(uint32_t now_ms);
	// NapiLog_Flush: the buffer to the file, when it holds anything.
	void flush(uint32_t now_ms);
	// NapiLog_Shutdown: the stop line when the log is on, then the flush.
	void shutdown(uint32_t now_ms);
	// What the buffer holds, not yet flushed.
	const std::string &pending() const { return buffer_; }

private:
	void write_entry(const char *text);

	ServerFileSink &files_;
	std::function<std::string()> stamp_;
	bool enabled_ = false;
	bool first_open_ = true; // the log's +16: the first open truncates
	std::string buffer_;
	uint32_t last_flush_ms_ = 0;
};

// Server_HandleAdminNetlogCommand over one console line: a line of no token is
// not taken; a line beginning `STAT` is taken whole; a first token NETLOG from
// the host itself (no slot: its own console, or the slot of its own local
// client) toggles the net log and, for a slot, answers it S2C 0x14
// [14][slot number]["NETLOG: SERVER SET TO <0|1>"] (reliable, 310 flushes),
// into `replies`; NETLOG is not taken either way, so the caller's arms go on.
// A host with no net log device (ctx.logs.net) toggles nothing.
// [orig: Server_HandleAdminNetlogCommand @0x5175D0 — the token count
//  @0x517621..0x51762C, the host test `!input || input+5` @0x51762F..0x51763B,
//  STAT @0x51764F..0x51765C, NETLOG @0x517670..0x5176B1, the reply
//  @0x5176C1..0x51771A, the return 0 @0x5175FD]
bool Server_HandleAdminNetlogCommand(NapiNPServerCtx &ctx, const std::string &line,
		const NapiNPConnection *slot, std::vector<ProtocolMessage> &replies);

// The authority's C2S 0x04 (the console line a session peer's
// Client_ProcessConsoleCommand queues, its NUL-terminated text): from a sender
// that holds a player slot, at most one a second by the slot's word the chat
// throttle shares; the text, cut at 1023 characters, runs the NETLOG handler,
// and a line it does not take goes back to the sender as S2C 0x06 [text NUL]
// (unreliable), which the client runs as its own command. `now_ms` is the
// host's clock.
// [orig: NapiNPServerMsg_HandleConsoleLine @0x5199D0 — the authority
//  @0x5199FA, the slot conn+352 -> +192 @0x519A01..0x519A18, the slot's
//  +100360 stamp @0x519A1E..0x519A3C, the copy @0x519A4D..0x519A72,
//  Server_HandleAdminNetlogCommand @0x519A7B, NetBuffer_WriteString and
//  SendFiltered(6, 0, 1) to the sender @0x519AB0..0x519AC5;
//  NapiNPClientMsg_HandleChatCommand @0x432BC0]
std::vector<ProtocolMessage> Server_HandleConsoleCommandMessage(NapiNPServerCtx &ctx,
		NapiNPConnection &sender, const std::vector<uint8_t> &payload, uint32_t now_ms);

// Entity_DumpUsageStatistics(name)'s text: "Object usage for <name>", the used
// counts of pools 0..3, then a line per items.def row past row 0 that one of
// the five pools' rows takes (its id, its count, its name), then a blank pair;
// '\n' as written (the file opens in text mode, so the disk takes CR LF).
// [orig: Entity_DumpUsageStatistics @0x40E620 — the formats @0x7C350C,
//  @0x7C34D8, @0x7C34A4, @0x7C3470, @0x7C3438, @0x7C3418, @0x7C3414; the five
//  pool walks over each row's +0x1C @0x40E725..0x40E755; the rows 1..gItemCount-1
//  by their +0x50 id and +0x00 name @0x40E757..0x40E7A2]
std::string entity_usage_text(const world::World &world, const def::DefItemsFile *items,
		const std::string &name);
// The file Entity_DumpUsageStatistics(name) writes: the name's first 27
// characters with everything from its first '.' replaced by ".nfo", or no file
// when it holds no '.' [orig: strncpy(.., 0x1B) and strstr(".") / strcpy(".nfo")
// @0x40E66E..0x40E6A6].
std::string entity_usage_file_name(const std::string &name);

} // namespace opennova::inmatch
