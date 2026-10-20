// The server's console (server_console.h): the client-less host's CHAT ring,
// its console chat and the /INOUT lines.
#include <runtime/inmatch/server_console.h>

#include <base/io/le.h>
#include <base/io/log.h> // io::logf (the /INOUT lines on the diagnostic channel)
#include <base/io/strutil.h>
#include <formats/def/def.h> // DefItemsFile (SAVENFO's rows)
#include <net/napi/session.h> // tokenize_quoted (String_TokenizeQuotedToArray)
#include <net/npwire/ingame_encode.h> // encode_chat_broadcast
#include <net/npwire/ingame_message_id.h>
#include <runtime/hud/feed_format.h> // chat_channel_sink / chat_channel_color / strip_inline_tags
#include <runtime/hud/hud_chat_entry.h>
#include <runtime/hud/hud_frame.h> // chat_wrap_text (HUD_WordWrapText)
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/world/world.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace opennova::inmatch {

void server_console_post(std::vector<ServerConsoleLine> &ring, const std::string &text,
		uint32_t argb) {
	ServerConsoleLine line;
	line.text = text.substr(0, kServerConsoleLineMax);
	line.color = argb;
	ring.push_back(std::move(line));
	while (ring.size() > kServerConsoleRingSlots) ring.erase(ring.begin());
}

namespace {

uint8_t *display_slot(ChatDisplayBuffer &buffer, size_t k) {
	return buffer.data() + (k - 1) * kChatDisplaySlotBytes;
}

const uint8_t *display_slot(const ChatDisplayBuffer &buffer, size_t k) {
	return buffer.data() + (k - 1) * kChatDisplaySlotBytes;
}

} // namespace

void chat_display_post(ChatDisplayBuffer &buffer, const ChatDisplayWrap &wrap,
		const std::string &text, uint32_t argb, int32_t life) {
	// The wrap NULs the line in place at each break; the segments are the runs.
	std::string message = text;
	int wrapped = 0;
	if (wrap.font != nullptr) {
		hud::GameFont font;
		font.set_font(wrap.font);
		wrapped = hud::chat_wrap_text(font, 1.0f, message, wrap.width, 0);
	}
	const size_t count = wrapped > 0 ? static_cast<size_t>(wrapped) : 1u;
	// The shift: slots 1..40-count move up by count, the oldest dropping off.
	if (count <= kChatDisplaySlotCount - 1) {
		for (size_t k = kChatDisplaySlotCount - count; k >= 1; --k)
			std::memcpy(display_slot(buffer, k + count), display_slot(buffer, k), kChatDisplaySlotBytes);
	}
	const size_t kept = std::min(count, kChatDisplaySlotCount);
	std::memset(display_slot(buffer, 1), 0, kept * kChatDisplaySlotBytes);
	// Slot 2's timer plus 186, added as the dword it is and compared signed
	// [orig: `add eax, 0BAh` then the signed compare with the life @0x498722..0x498734].
	const uint32_t staggered = io::read_u32_le(display_slot(buffer, 2) + kChatDisplayTimerOffset) +
			static_cast<uint32_t>(kChatDisplayStagger);
	const uint32_t timer =
			static_cast<int32_t>(staggered) > life ? staggered : static_cast<uint32_t>(life);
	io::write_u32_le(display_slot(buffer, 1) + kChatDisplayTimerOffset, timer);
	// The segments, the first into slot `count`, the last into slot 1.
	const char *text_bytes = message.c_str();
	size_t cursor = 0;
	for (size_t k = count; k >= 1; --k) {
		size_t copy = std::strlen(text_bytes + cursor) + 1;
		if (copy >= kChatDisplayCopyMax + 1) copy = kChatDisplayCopyMax;
		if (k <= kChatDisplaySlotCount) {
			uint8_t *slot = display_slot(buffer, k);
			uint8_t *write = slot;
			if (k != count) {
				slot[0] = ' ';
				slot[1] = ' ';
				slot[2] = 0;
				write = slot + 2;
			}
			std::memcpy(write, text_bytes + cursor, copy);
			io::write_u32_le(slot + kChatDisplayColorOffset, argb);
		}
		cursor += copy;
	}
}

void chat_display_tick(ChatDisplayBuffer &buffer) {
	for (size_t k = 1; k <= kChatDisplaySlotCount; ++k) {
		// A nonzero dword steps down by one, wrapping as `add ecx, -1` does.
		uint8_t *timer = display_slot(buffer, k) + kChatDisplayTimerOffset;
		const uint32_t value = io::read_u32_le(timer);
		if (value != 0) io::write_u32_le(timer, value - 1u);
	}
}

std::string chat_display_get(const ChatDisplayBuffer &buffer) {
	std::string out;
	const uint8_t *end = buffer.data() + ChatDisplayBuffer::size();
	for (size_t k = kChatDisplaySlotCount; k >= 1; --k) {
		const uint8_t *begin = display_slot(buffer, k);
		const uint8_t *nul = std::find(begin, end, uint8_t{0});
		if (nul == begin) continue;
		out.append(reinterpret_cast<const char *>(begin), static_cast<size_t>(nul - begin));
		out += "\r\n";
	}
	return out;
}

void server_console_add(NapiNPServerCtx &ctx, const std::string &text, uint32_t argb) {
	server_console_post(ctx.console_chat, text, argb);
	chat_display_post(ctx.console_chat_display, ctx.console_chat_wrap, text, argb, kChatDisplayLife);
}

void server_console_clear(NapiNPServerCtx &ctx) {
	ctx.console_chat.clear();
	ctx.console_chat_display.clear();
}

void server_console_dispatch(NapiNPServerCtx &ctx, int channel, const std::string &text) {
	// The \x07 escape strip (Chat_ProcessEscapeCodes @0x424580 @0x42b94f, its
	// interface sounds) is not ported here, as on the client's ring.
	if (hud::chat_channel_sink(channel) != hud::ChatSink::Chat) return;
	server_console_add(ctx, text, hud::chat_channel_color(channel));
}

std::vector<ServerConsoleLine> server_console_rows(const std::vector<ServerConsoleLine> &ring) {
	std::vector<ServerConsoleLine> rows(kServerConsoleShownLines);
	const size_t count = ring.size();
	for (size_t row = 0; row < kServerConsoleShownLines; ++row) {
		// Row 0 shows raw slot 3, row 3 slot 0 (the newest).
		const size_t slot = kServerConsoleShownLines - 1 - row;
		if (slot >= count) continue;
		ServerConsoleLine line = ring[count - 1 - slot];
		if (line.color == 0u) line.color = 0xFFFFFFFFu;
		rows[row] = std::move(line);
	}
	return rows;
}

hud::ChatSendResult Server_SendConsoleChat(NapiNPServerCtx &ctx, int dispatch,
		std::string &text, uint32_t frame) {
	using Result = hud::ChatSendResult;
	if (dispatch != hud::kChatDispatchGlobal || !ctx.is_authority || ctx.is_mp_session_peer)
		return Result::Refused;
	// `(!g_DeathScreenActive || g_SpawnSuccessGate)` holds on a host with no
	// player of its own (its death screen never rises) [orig: @0x49a931].
	if (text.empty()) return Result::Refused;
	if (!chat_flood_check(ctx.console_chat_flood, text, frame)) return Result::Flooded;
	const std::string stripped = hud::strip_inline_tags(text); // [orig: @0x49a971]
	ChatBroadcast chat;
	chat.channel = 10;        // [orig: `push 0Ah` ahead of @0x49aa18]
	chat.sender_slot = 0xFF;  // [orig: `push 0FFh`]
	chat.text = stripped;
	const std::vector<uint8_t> body = encode_chat_broadcast(chat);
	for (NapiNPConnection &c : ctx.np_protocol.connection_list) {
		if (!active_player_recipient(c)) continue; // send mask 0x80
		c.link.transport->host_send(s2c::CHAT_BROADCAST, body, /*reliable=*/true, 0, false,
				/*retention=*/310);
	}
	server_console_dispatch(ctx, 10, text); // [orig: @0x49aa2a, the unstripped line]
	return Result::Broadcast;
}

uint32_t server_console_submit(NapiNPServerCtx &ctx, hud::ChatEntry &entry,
		const std::string &line, uint32_t frame, const hud::GameTextLookup &gametext) {
	using namespace hud::chat_entry_event;
	// The facts the talk arms read on a host with no player of its own: no
	// death screen, no reset hold (a client's S2C 0x25 sets it), no vehicle.
	hud::ChatEntryFacts f;
	f.in_session = ctx.is_in_session != 0;
	f.team_game = (ctx.config.game_type & 0x10000u) != 0;
	f.novaworld = ctx.transport_mode == NetworkType::NovaWorld;
	f.authority = ctx.is_authority != 0;
	f.mp_session_peer = ctx.is_mp_session_peer != 0;
	f.frame = frame;
	const hud::ChatSender send = [&ctx, frame](int dispatch, std::string &text) {
		return Server_SendConsoleChat(ctx, dispatch, text, frame);
	};
	uint32_t events = entry.dispatch(hud::kChatDispatchGlobal, f, gametext);
	if (!entry.capturing()) return events;
	for (const char c : line) events |= entry.key(static_cast<unsigned char>(c), 0, f, gametext, send);
	if (entry.capturing()) events |= entry.key(13, 0, f, gametext, send);
	if ((events & kFloodEcho) != 0)
		server_console_add(ctx, entry.echo_text(), hud::chat_dispatch_flood_color(entry.echo_dispatch()));
	return events;
}

namespace {

std::string host_address(const NapiNPServerCtx &ctx) {
	// [orig: @0x61e6c6..0x61e6fd — the protocol's socket address, else the
	//  placeholder]
	if (!ctx.local_address_known) return "???.???.???.???:?????";
	return peer_addr_to_string(ctx.local_address); // Napi_FormatAddress "%s:%ld" @0x62df22
}

const char *connection_kind(const NapiNPConnection &conn) {
	// is_server (+0x2B) for a node the server created (type 1), is_client
	// (+0x2A) for type 2 [orig: CNapiNPConnection_Create @0x62ad7b..0x62ae95].
	if (conn.type == NapiNPConnection::kTypeServerSide) return "SERVER ";
	if (conn.type == NapiNPConnection::kTypeClientSide) return "CLIENT ";
	return "";
}

} // namespace

std::string inout_host_line(const NapiNPServerCtx &ctx, bool started) {
	// [orig: "%s : HOST STARTED \"%s\"." @0x7dfa7c / "%s : HOST STOPPED \"%s\"."
	//  @0x7dfaac over the protocol's session name (+0x288)]
	return host_address(ctx) + (started ? " : HOST STARTED \"" : " : HOST STOPPED \"") +
			ctx.np_protocol.session_name + "\".";
}

std::string inout_player_added_line(const NapiNPConnection &conn) {
	// [orig: CNapiNPConnection_LogPlayerAdded @0x61e860 — the node's address
	//  @0x61e8a6 and name (+0x80) @0x61e909]
	return peer_addr_to_string(conn.peer) + " : " + connection_kind(conn) + "PLAYER ADDED \"" +
			conn.player_name + "\".";
}

std::string napi_log_stamp(std::time_t t) {
	// [orig: NapiLog_WriteEntry @0x62EE7C..0x62EEA8 over Napi_GetLocalTime's
	//  _localtime64: the year with its century, the month from 1]
	std::tm local{};
#if defined(_WIN32)
	localtime_s(&local, &t);
#else
	localtime_r(&t, &local);
#endif
	char text[64];
	std::snprintf(text, sizeof(text), "%4.4ld-%2.2ld-%2.2ld %2.2ld:%2.2ld:%2.2ld: ",
			static_cast<long>(local.tm_year + 1900), static_cast<long>(local.tm_mon + 1),
			static_cast<long>(local.tm_mday), static_cast<long>(local.tm_hour),
			static_cast<long>(local.tm_min), static_cast<long>(local.tm_sec));
	return text;
}

NapiLog::NapiLog(ServerFileSink &files, std::string file_name)
	: files_(files), file_name_(std::move(file_name)),
	  stamp_([] { return napi_log_stamp(std::time(nullptr)); }) {}

void NapiLog::write_entry(const char *text) {
	// Mode 3: the stamp (flag 4) and the line into the buffer, CR LF after it.
	buffer_ += stamp_();
	buffer_ += text;
	buffer_ += "\r\n";
}

void NapiLog::set_enabled(bool on) {
	// [orig: NapiLog_SetEnabled @0x62F2B0 — the same state returns @0x62F2BD; the
	//  stop line before the clear @0x62F2C5..0x62F2CF, the set before the start
	//  line @0x62F2D7..0x62F2E0]
	if (on == enabled_) return;
	if (on) {
		enabled_ = true;
		write_entry("NapiLog Started!");
	} else {
		write_entry("NapiLog Stopped!");
		enabled_ = false;
	}
}

void NapiLog::flush(uint32_t now_ms) {
	// [orig: NapiLog_Flush @0x62F1A0 — a nonempty buffer only; the open, the
	//  write, the close; the buffer emptied whether the open held or not; the
	//  stamp of the flush always]
	if (!buffer_.empty()) {
		// The first open truncates the file (0x20 in the open's flags while
		// the log's +16 stands), every later one appends; a failed open leaves
		// +16 as it was [orig: NapiLog_EnsureOpen @0x62EDD8..0x62EDFB].
		if (files_.write(file_name_, buffer_, /*append=*/!first_open_)) first_open_ = false;
		buffer_.clear();
	}
	last_flush_ms_ = now_ms;
}

void NapiLog::flush_if_expired(uint32_t now_ms) {
	// [orig: NapiLog_FlushIfExpired @0x62F2F0 — `now - last >= interval`]
	if (now_ms - last_flush_ms_ >= kNapiLogFlushIntervalMs) flush(now_ms);
}

void NapiLog::shutdown(uint32_t now_ms) {
	// [orig: NapiLog_Shutdown @0x62F310 — the stop line and the clear when on,
	//  then the flush]
	if (enabled_) {
		write_entry("NapiLog Stopped!");
		enabled_ = false;
	}
	flush(now_ms);
}

void write_inout_line(const NapiNPServerCtx &ctx, const std::string &line) {
	io::logf(io::LogLevel::kInfo, "%s", line.c_str());
	if (ctx.logs.inout != nullptr && ctx.logs.inout->enabled()) ctx.logs.inout->write(line);
}

void open_inout_log(const NapiNPServerCtx &ctx) {
	if (ctx.logs.inout != nullptr) ctx.logs.inout->set_enabled(ctx.logs.inout_switch);
}

namespace {

// The chat throttle's interval, which the console line's shares [orig: the
// slot's +100360 word, `last + 1000` @0x519A36].
constexpr uint32_t kConsoleLineThrottleMs = 1000;
// The copy's bound: 1023 characters and the NUL [orig: the 1024 count
// @0x519A53].
constexpr size_t kConsoleLineMaxChars = 1023;
// NETLOG's reply chat channel and lifetime [orig: NetPacket_WriteTwoBytesAndCString
// (.., slot+20, 14, ..) @0x517715; SendFiltered(0x14, 1, 310) @0x51771A].
constexpr uint8_t kNetlogReplyChannel = 14;
constexpr uint32_t kNetlogReplyRetentionFlushes = 310;

} // namespace

bool Server_HandleAdminNetlogCommand(NapiNPServerCtx &ctx, const std::string &line,
		const NapiNPConnection *slot, std::vector<ProtocolMessage> &replies) {
	const std::vector<std::string> tokens = tokenize_quoted(line);
	if (line.empty() || tokens.empty()) return false;
	const bool host_itself = slot == nullptr || slot_is_local(*slot);
	if (strutil::starts_with_icase(line, "STAT")) return true;
	if (!strutil::iequals(tokens[0], "NETLOG") || !host_itself || ctx.logs.net == nullptr) return false;
	NapiLog &log = *ctx.logs.net;
	log.set_enabled(!log.enabled());
	if (slot != nullptr) {
		char text[64];
		std::snprintf(text, sizeof(text), "NETLOG: SERVER SET TO %ld", static_cast<long>(log.enabled() ? 1 : 0));
		ChatBroadcast reply;
		reply.channel = kNetlogReplyChannel;
		reply.sender_slot = static_cast<uint8_t>(slot->reply.player_slot);
		reply.text = text;
		ProtocolMessage message = make_protocol_message(s2c::CHAT_BROADCAST, encode_chat_broadcast(reply));
		message.retention_flushes = kNetlogReplyRetentionFlushes;
		replies.push_back(std::move(message));
	}
	return false;
}

void Server_HandleConsoleCommandMessage(NapiNPServerCtx &ctx, NapiNPConnection &sender,
		const std::vector<uint8_t> &payload, uint32_t now_ms, std::vector<ProtocolMessage> &replies) {
	if (ctx.is_authority == 0 || !player_slot_active(sender)) return;
	const uint32_t last = sender.reply.chat_last_ms;
	if (last != 0 && now_ms < last + kConsoleLineThrottleMs) return;
	sender.reply.chat_last_ms = now_ms;
	std::string line;
	for (size_t i = 0; i < payload.size() && line.size() < kConsoleLineMaxChars && payload[i] != 0; ++i)
		line.push_back(static_cast<char>(payload[i]));
	if (Server_HandleAdminNetlogCommand(ctx, line, &sender, replies)) return;
	std::vector<uint8_t> body(line.begin(), line.end());
	body.push_back(0);
	ProtocolMessage echo = make_protocol_message(s2c::CLIENT_CONSOLE_COMMAND, std::move(body));
	echo.reliable = false;
	replies.push_back(std::move(echo));
}

std::string entity_usage_file_name(const std::string &name) {
	std::string file = name.substr(0, 27);
	const size_t dot = file.find('.');
	if (dot == std::string::npos) return std::string();
	return file.substr(0, dot) + ".nfo";
}

std::string entity_usage_text(const world::World &world, const def::DefItemsFile *items,
		const std::string &name) {
	const size_t item_count = items != nullptr ? items->count : 0;
	std::vector<int32_t> counts(item_count, 0);
	// Every row of the five pools below its used count: a live row counts by its
	// ItemTypeIndex, a freed one is ItemTypeIndex 0, which no line prints.
	for (int pool = 0; pool < world::EntityRegistry::kPoolCount; ++pool) {
		world.registry.for_each_in_pool(pool, [&](const world::Entity &e) {
			if (e.item_type_index > 0 && static_cast<size_t>(e.item_type_index) < counts.size())
				++counts[static_cast<size_t>(e.item_type_index)];
		});
	}
	std::string out = "Object usage for " + name;
	char line[512];
	std::snprintf(line, sizeof(line), "\nNumber of OITEMS (people and players)        :\t%d\n",
			static_cast<int>(world.registry.pool_used(0)));
	out += line;
	std::snprintf(line, sizeof(line), "\nNumber of MITEMS (vehicles and moving things):\t%d\n",
			static_cast<int>(world.registry.pool_used(1)));
	out += line;
	std::snprintf(line, sizeof(line), "\nNumber of DITEMS (static things)             :\t%d\n",
			static_cast<int>(world.registry.pool_used(2)));
	out += line;
	std::snprintf(line, sizeof(line), "\nNumber of MARKERS (You Know Markers)         :\t%d\n\n",
			static_cast<int>(world.registry.pool_used(3)));
	out += line;
	for (size_t index = 1; index < item_count; ++index) {
		if (counts[index] == 0) continue;
		const def::DefItemDef &row = items->entries[index];
		std::snprintf(line, sizeof(line), "type id-%07d  count-%04d  %s \n", row.id, counts[index],
				row.display_name);
		out += line;
	}
	out += "\n\n";
	return out;
}

std::string inout_player_removed_line(const NapiNPConnection &conn) {
	// [orig: CNapiNPConnection_LogPlayerRemoved @0x61e940 — the disconnect
	//  record's DS/DC/DP1/DP2 (+0x658..+0x664), DSTR (+0x668), DPC (+0x6E8),
	//  DDSTR (+0x6EC) @0x61e99e..0x61e9c6, read whether latched or not]
	const DisconnectEvent &e = conn.disconnect_event;
	char tail[512];
	std::snprintf(tail, sizeof(tail), "[%ld,%ld,%ld,%ld,\"%s\",%ld,\"%s\"]",
			static_cast<long>(e.ds), static_cast<long>(e.dc), static_cast<long>(e.dp1),
			static_cast<long>(e.dp2), e.dstr.c_str(), static_cast<long>(e.dpc), e.ddstr.c_str());
	return peer_addr_to_string(conn.peer) + " : " + connection_kind(conn) + "PLAYER REMOVED \"" +
			conn.player_name + "\". " + tail;
}

} // namespace opennova::inmatch
