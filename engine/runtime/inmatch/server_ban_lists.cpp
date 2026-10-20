// The authority's two ban lists (server_ban_lists.h).
#include <runtime/inmatch/server_ban_lists.h>

#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_tick.h> // Server_StageHostDisconnect

#include <runtime/inmatch/punt_log.h> // napi_time_string (the /INOUT line's stamp), kNapiLineEnding
#include <runtime/inmatch/server_console.h> // kInoutLogFileName

#include <base/io/crt_ftol.h>
#include <net/npwire/ingame_encode.h>     // encode_chat_broadcast (S2C 0x14)
#include <net/npwire/ingame_message_id.h>
#include <base/io/log.h>
#include <net/napi/session.h> // tokenize_quoted (String_TokenizeQuotedToArray)
#include <net/novaworld/join_identity.h> // decrypt_join_cookie_field

#include <base/io/strutil.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace opennova::inmatch {

namespace {

// The validator's cookie reads: each field decrypts into a 512-byte buffer, and the NAMEINFO
// name keeps 16 characters.
// [orig: Server_ValidatePlayerJoinRequest - `push 200h` @0x51262C / @0x5126E2, the cut
//  `cmp eax, 10h` @0x51268B and the NUL store @0x512690]
constexpr std::size_t kJoinCookieFieldBytes = 512;
constexpr std::size_t kJoinCookieNameChars = 16;

std::string file_path(const ServerBanLists &bans, const char *name) {
	if (!bans.directory.has_value() || bans.directory->empty()) return name;
	std::string path = *bans.directory;
	if (path.back() != '/' && path.back() != '\\') path.push_back('/');
	return path + name;
}

// PlayerState_GetByIndex: the slot at `index` below the table's capacity, as the port keeps
// it: the connection holding that player slot (null for a free row).
// [orig: PlayerState_GetByIndex @0x500850]
NapiNPConnection *slot_at(NapiNPServerCtx &ctx, int32_t index) {
	if (index < 0 || index >= static_cast<int32_t>(ctx.config.total_player_slot_capacity())) return nullptr;
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list)
		if (player_slot_active(conn) && conn.reply.player_slot == static_cast<uint32_t>(index)) return &conn;
	return nullptr;
}

// CNapiNPConnection_QueueDisconnectEvent(connection, "", 33, detail): the disconnect description
// with reason 33. [orig: CNapiNPConnection_QueueDisconnectEvent @0x4C7EF0]
void punt(NapiNPConnection &conn, const char *detail) {
	DisconnectEvent event;
	event.ds = 1;
	event.dc = 2;
	event.dpc = 33;
	event.ddstr = detail;
	Server_StageHostDisconnect(conn, event);
}

// The handler's sprintf lines: one %ld (the index) and up to two strings.
std::string format_line(const char *format, long index, const std::string &a = {},
		const std::string &b = {}) {
	char buffer[512];
	std::snprintf(buffer, sizeof buffer, format, index, a.c_str(), b.c_str());
	return buffer;
}

// Server_SendChatMessageToPlayer(slot, 3, text): nothing for an empty text, else S2C 0x14
// [3][the slot's own index][text], reliable with its 310-flush retention, to that slot
// alone (mask 32); the sender's own line joins its dispatch replies.
// [orig: Server_SendChatMessageToPlayer @0x508050 — the strlen test @0x508074, mask 32
//  @0x508080, the slot @0x50808A, its index slot+0x14 @0x50808F,
//  NetPacket_WriteTwoBytesAndCString @0x50809D, userParam 0x136 @0x5080AB, the send @0x5080BE]
void send_chat_to_player(NapiNPConnection &to, const std::string &text,
		std::vector<ProtocolMessage> *replies) {
	constexpr int8_t kPlayerChatChannel = 3;
	constexpr uint32_t kChatRetentionFlushes = 0x136;
	if (text.empty()) return;
	ChatBroadcast chat;
	chat.channel = kPlayerChatChannel;
	chat.sender_slot = static_cast<uint8_t>(to.reply.player_slot);
	chat.text = text;
	std::vector<uint8_t> body = encode_chat_broadcast(chat);
	if (replies != nullptr) {
		ProtocolMessage message = make_protocol_message(s2c::CHAT_BROADCAST, std::move(body));
		message.retention_flushes = kChatRetentionFlushes;
		replies->push_back(std::move(message));
		return;
	}
	if (to.link.transport != nullptr)
		to.link.transport->host_send(s2c::CHAT_BROADCAST, std::move(body), true, 0, false,
				kChatRetentionFlushes);
}

} // namespace

// [orig: BanList_InitFromMission @0x5098F0 — LinkedList_FreeAll @0x5098F5; the authority,
//  in-session and NovaWorld gates; BanList_LoadFromFile("banlist.txt"); on no list
//  BanListHeader_Create and BanList_SaveToFileWithHeader]
void Server_ReloadPcidBanList(NapiNPServerCtx &ctx) {
	ctx.bans.pcids.reset();
	if (ctx.is_authority == 0 || ctx.is_in_session == 0 || ctx.transport_mode != NetworkType::NovaWorld)
		return;
	if (ctx.bans.directory.has_value())
		ctx.bans.pcids = banlist::load_pcid_list(file_path(ctx.bans, banlist::kPcidFileName));
	if (!ctx.bans.pcids.has_value()) {
		ctx.bans.pcids = banlist::PcidBanList{};
		Server_SavePcidBanList(ctx);
	}
}

// [orig: Server_InitNewRoundState — g_BannedNameCount = 0 @0x51CB2F, dword_C8FF24 = 0
//  @0x51CB35, File_ParseASCIIFileWithCallback("banned.txt", BanList_ParseIPEntry) @0x51CB3B]
void Server_ReloadAddressBanList(NapiNPServerCtx &ctx) {
	ctx.bans.addresses = banlist::AddressBanList{};
	ctx.bans.addresses_dirty = false;
	if (ctx.bans.directory.has_value())
		banlist::load_address_list(file_path(ctx.bans, banlist::kAddressFileName), ctx.bans.addresses);
}

void Server_SavePcidBanList(const NapiNPServerCtx &ctx) {
	if (!ctx.bans.pcids.has_value() || !ctx.bans.directory.has_value()) return;
	banlist::save_pcid_list(file_path(ctx.bans, banlist::kPcidFileName), *ctx.bans.pcids);
}

void Server_SaveAddressBanList(const NapiNPServerCtx &ctx) {
	if (ctx.bans.addresses.entries.empty() || !ctx.bans.addresses_dirty) return;
	if (!ctx.bans.directory.has_value()) return;
	banlist::save_address_list(file_path(ctx.bans, banlist::kAddressFileName), ctx.bans.addresses);
}

bool Server_PcidBanned(const NapiNPServerCtx &ctx, std::string_view pcid) {
	return ctx.bans.pcids.has_value() && banlist::find(*ctx.bans.pcids, pcid) != nullptr;
}

bool Server_NameBanned(const NapiNPServerCtx &ctx, std::string_view name) {
	for (const banlist::AddressBan &ban : ctx.bans.addresses.entries)
		if (ban.name == name) return true;
	return false;
}

uint32_t Server_ValidateJoinAccount(NapiNPServerCtx &ctx, NapiNPConnection &conn) {
	std::string local_address;
	if (get_local_address(ctx, local_address)) {
		// The cookie legs, each refusal a description punt (D-NET-702).
		if (!conn.join_cookie_present) return 23;
		std::vector<uint8_t> field;
		if (!decrypt_join_cookie_field(conn.join_identity_pairs, local_address + "NAMEINFO",
					ctx.cookie_key_table, kJoinCookieFieldBytes, field))
			return 24;
		std::string name(reinterpret_cast<const char *>(field.data()));
		if (name.size() > kJoinCookieNameChars) name.resize(kJoinCookieNameChars);
		if (conn.player_name != name) conn.player_name = name;
		if (!decrypt_join_cookie_field(conn.join_identity_pairs, local_address + "PCID",
					ctx.cookie_key_table, kJoinCookieFieldBytes, field))
			return 25;
		if (Server_PcidBanned(ctx, conn.account.pcid))
			return 29;
		if (ctx.config.punt_duplicate_pcids != 0) {
			const std::string &pcid = conn.account.pcid;
			uint32_t holders = 0;
			for (const NapiNPConnection &other : ctx.np_protocol.connection_list)
				if (strutil::iequals(other.account.pcid, pcid))
					++holders;
			if (holders > 1) {
				const std::string stamp = napi_time_string(std::time(nullptr));
				for (NapiNPConnection &other : ctx.np_protocol.connection_list) {
					if (!strutil::iequals(other.account.pcid, pcid))
						continue;
					if (&other != &conn && !slot_is_local(other))
						other.duplicate_pcid_kick_pending = true;
					// The /INOUT line for every holder, the joiner and the local one too,
					// appended straight to _inout.txt (not through the protocol's log) under
					// the switch [orig: "%s : REJECTING connection \"%s\" because pcid
					//  \"%s\" matches \"%s\"!" @0x7CFAE0 -> NapiFile_AppendLine("_inout.txt")
					//  @0x512865, gated on g_InoutLogEnabled @0x5127F7].
					const std::string line = stamp + " : REJECTING connection \"" +
							peer_addr_to_string(other.peer) + "\" because pcid \"" + pcid +
							"\" matches \"" + other.account.pcid + "\"!";
					io::logf(io::LogLevel::kInfo, "%s", line.c_str());
					if (ctx.logs.inout_switch && ctx.logs.files != nullptr)
						ctx.logs.files->write(kInoutLogFileName, line + kNapiLineEnding, /*append=*/true);
				}
				return 30;
			}
		}
		if (join_tickets_required(ctx)) {
			const std::string ticket = local_address + "JOINTICKET";
			bool named = false;
			for (const auto &pair : conn.join_identity_pairs)
				named = named || strutil::iequals(pair.first, ticket);
			if (!named) return 28;
		}
	}
	if (ctx.is_in_session != 0 && Server_NameBanned(ctx, conn.player_name)) return 31;
	return 0;
}

void Server_KickDuplicatePcids(NapiNPServerCtx &ctx, bool quality_boundary) {
	// [orig: Server_TickUpdate - the 0x79 countdown's zero @0x51E1C5 gates the call @0x51E20C]
	if (!quality_boundary)
		return;
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (!conn.duplicate_pcid_kick_pending || slot_is_local(conn))
			continue;
		// DS from the node's role bytes: +0x2B (is_server) 1, +0x2A (is_client) 2.
		uint32_t role = 0;
		if (conn.type == NapiNPConnection::kTypeServerSide) role = 1;
		else if (conn.type == NapiNPConnection::kTypeClientSide) role = 2;
		latch_disconnect_event(conn,
				make_disconnect_event(
						role, 2, 0, 0, "N.C:SPTWNTBP[" + conn.account.pcid + "]", 0, ""));
		conn.pending_disconnect = true;
	}
}

bool Server_AddressBanned(const NapiNPServerCtx &ctx, uint32_t address) {
	for (const banlist::AddressBan &ban : ctx.bans.addresses.entries)
		if (ban.address == address) return true;
	return false;
}

// [orig: Server_HandleBanPuntCommand @0x50B380. The sender is authorized when there is none
//  (the console's 0) or it is the local slot (+5) @0x50B3BE..0x50B3CA; every chat reply is
//  gated on a sender, so the console's line draws none. The line through
//  String_TokenizeQuotedToArray @0x616D60 (the call @0x50B3DE); fewer than one token is not
//  handled. With two or
//  more tokens:
//  - BAN @0x50B41B: unauthorized or with no list, handled with nothing @0x50B43A; else `atol`
//    of token 1 range-checked 0..capacity (inclusive, "Player number %ld out of range."
//    @0x50B5F3), the row by index (none: "Player #%ld doesn't exist." @0x50B480; inactive:
//    "Player #%ld not used." @0x50B493), the host's own refused ("Cannot BAN the host
//    player." @0x50B4CE), no PCID ("Player #%ld \"%s\" doesn't have a PCID." @0x50B557), else
//    LinkedList_AddEntry with the slot name @0x50B584, saved @0x50B59F, and "Banned Player
//    #%ld \"%s\"." to the BANNED player @0x50B5BB..0x50B5CB.
//  - UNBAN @0x50B60F: the same gates @0x50B62E, "Player #%ld out of range." @0x50B824, the
//    same row tests (@0x50B67C, @0x50B493 via @0x50B696, "Cannot UNBAN the host player."
//    @0x50B6D7, @0x50B74A), LinkedList_FindByName @0x50B77E: listed -> removed, saved
//    @0x50B79C, "Unbanned Player #%ld \"%s\"." @0x50B7B7; else "Player #%ld \"%s\" with PCID
//    of \"%s\" doesn't appear in your banned list." @0x50B7F0, each to the sender.
//  Then, when the line was neither, authorized only @0x50B845:
//  - PUNT (exactly two tokens): ALL -> every active slot not local, not a bot (+96483) and not
//    dropping (+96481) punted reason 33 "S.C:SPA"; else `atol` 0..capacity, that slot under the
//    same tests punted "S.C:SP#"; handled.
//  - BANDWIDTH (two tokens: g_EntitySendBudget = clamp(atol / 5, 100, 1600); one token: 600);
//    not handled, so the console's chain goes on.
//  A row with no net player ("not connected to a network.", @0x50B511 / @0x50B716) is a bot's,
//  which this host never seats.]
bool Server_HandleBanPuntCommand(NapiNPServerCtx &ctx, std::string_view line,
		NapiNPConnection *sender, std::vector<ProtocolMessage> *sender_replies) {
	const std::vector<std::string> tokens = tokenize_quoted(line);
	if (tokens.empty()) return false;
	const bool authorized = sender == nullptr || slot_is_local(*sender);
	const int32_t capacity = static_cast<int32_t>(ctx.config.total_player_slot_capacity());
	// Server_SendChatMessageToPlayer(slot, 3, text), sender-gated by every caller here.
	const auto tell = [&](NapiNPConnection &to, const std::string &text) {
		if (sender != nullptr) send_chat_to_player(to, text, &to == sender ? sender_replies : nullptr);
	};
	const bool ban = tokens.size() > 1 && strutil::iequals(tokens[0], "BAN");
	const bool unban = !ban && tokens.size() > 1 && strutil::iequals(tokens[0], "UNBAN");
	if (ban || unban) {
		if (!authorized || !ctx.bans.pcids.has_value()) return true;
		const int32_t index = io::retail_atol(tokens[1].c_str());
		if (index < 0 || index > capacity) {
			if (sender != nullptr)
				tell(*sender, format_line(ban ? "Player number %ld out of range."
				                              : "Player #%ld out of range.", index));
			return true;
		}
		if (index == capacity) {
			if (sender != nullptr) tell(*sender, format_line("Player #%ld doesn't exist.", index));
			return true;
		}
		NapiNPConnection *slot = slot_at(ctx, index);
		if (slot == nullptr) {
			if (sender != nullptr) tell(*sender, format_line("Player #%ld not used.", index));
			return true;
		}
		const std::string &name = slot->reply.player_name;
		if (slot_is_local(*slot)) {
			if (sender != nullptr)
				tell(*sender, ban ? "Cannot BAN the host player." : "Cannot UNBAN the host player.");
			return true;
		}
		if (slot->account.pcid.empty()) {
			if (sender != nullptr)
				tell(*sender, format_line("Player #%ld \"%s\" doesn't have a PCID.", index, name));
			return true;
		}
		if (ban) {
			if (banlist::add(*ctx.bans.pcids, slot->account.pcid, name)) {
				Server_SavePcidBanList(ctx);
				tell(*slot, format_line("Banned Player #%ld \"%s\".", index, name));
			}
			return true;
		}
		const std::string pcid = slot->account.pcid;
		if (banlist::remove(*ctx.bans.pcids, pcid)) {
			Server_SavePcidBanList(ctx);
			if (sender != nullptr) tell(*sender, format_line("Unbanned Player #%ld \"%s\".", index, name));
		} else if (sender != nullptr) {
			tell(*sender, format_line(
					"Player #%ld \"%s\" with PCID of \"%s\" doesn't appear in your banned list.",
					index, name, pcid));
		}
		return true;
	}
	if (!authorized) return false;
	if (tokens.size() == 2 && strutil::iequals(tokens[0], "BANDWIDTH")) {
		const int32_t budget = io::retail_atol(tokens[1].c_str()) / 5;
		ctx.config.entity_send_budget = static_cast<uint32_t>(std::min(std::max(budget, 100), 1600));
		return false;
	}
	if (tokens.size() == 2 && strutil::iequals(tokens[0], "PUNT")) {
		// The bot (+96483) and dropping (+96481) bytes have no writer on this host.
		if (strutil::iequals(tokens[1], "ALL")) {
			for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
				if (!player_slot_active(conn) || slot_is_local(conn)) continue;
				if (conn.reply.player_slot >= static_cast<uint32_t>(capacity)) continue;
				punt(conn, "S.C:SPA");
			}
			return true;
		}
		const int32_t index = io::retail_atol(tokens[1].c_str());
		if (index < 0 || index > capacity) return true;
		NapiNPConnection *slot = slot_at(ctx, index);
		if (slot != nullptr && !slot_is_local(*slot)) punt(*slot, "S.C:SP#");
		return true;
	}
	if (tokens.size() == 1 && strutil::iequals(tokens[0], "BANDWIDTH")) ctx.config.entity_send_budget = 600;
	return false;
}

} // namespace opennova::inmatch
