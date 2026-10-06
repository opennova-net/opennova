// The server's console (server_console.h): the client-less host's CHAT ring,
// its console chat and the /INOUT lines.
#include <runtime/inmatch/server_console.h>

#include <net/npwire/ingame_encode.h> // encode_chat_broadcast
#include <net/npwire/ingame_message_id.h>
#include <runtime/hud/feed_format.h> // chat_channel_sink / chat_channel_color / strip_inline_tags
#include <runtime/hud/hud_chat_entry.h>
#include <runtime/inmatch/napi_np_server_ctx.h>

#include <cstdio>

namespace opennova::inmatch {

void server_console_post(std::vector<ServerConsoleLine> &ring, const std::string &text,
		uint32_t argb) {
	ServerConsoleLine line;
	line.text = text.substr(0, kServerConsoleLineMax);
	line.color = argb;
	ring.push_back(std::move(line));
	while (ring.size() > kServerConsoleRingSlots) ring.erase(ring.begin());
}

void server_console_dispatch(NapiNPServerCtx &ctx, int channel, const std::string &text) {
	// The \x07 escape strip (Chat_ProcessEscapeCodes @0x424580 @0x42b94f, its
	// interface sounds) is not ported here, as on the client's ring.
	if (hud::chat_channel_sink(channel) != hud::ChatSink::Chat) return;
	server_console_post(ctx.console_chat, text, hud::chat_channel_color(channel));
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
		server_console_post(ctx.console_chat, entry.echo_text(),
				hud::chat_dispatch_flood_color(entry.echo_dispatch()));
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
