// The authority's two ban lists (server_ban_lists.h).
#include <runtime/inmatch/server_ban_lists.h>

#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_tick.h> // Server_StageHostDisconnect

#include <runtime/inmatch/punt_log.h> // napi_time_string (the /INOUT line's stamp)

#include <base/io/crt_ftol.h>
#include <base/io/log.h>
#include <net/napi/session.h> // tokenize_quoted (String_TokenizeQuotedToArray)

#include <base/io/strutil.h>

#include <algorithm>
#include <ctime>
#include <string>
#include <vector>

namespace opennova::inmatch {

namespace {

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

// CNapiNPConnection_SendChatMessage(connection, "", 33, detail): the disconnect description
// with reason 33. [orig: CNapiNPConnection_SendChatMessage @0x4C7EF0]
void punt(NapiNPConnection &conn, const char *detail) {
	DisconnectEvent event;
	event.ds = 1;
	event.dc = 2;
	event.dpc = 33;
	event.ddstr = detail;
	Server_StageHostDisconnect(conn, event);
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

uint32_t Server_ValidateJoinBans(
		NapiNPServerCtx &ctx, NapiNPConnection &conn, bool pcid_decrypted) {
	std::string local_address;
	if (get_local_address(ctx, local_address)) {
		if (Server_PcidBanned(ctx, conn.account.pcid))
			return 29;
		if (pcid_decrypted && ctx.config.punt_duplicate_pcids != 0) {
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
					// The /INOUT line for every holder, the joiner and the local one too: the
					// port writes the protocol's /INOUT lines on the diagnostic channel
					// (D-NET-356) [orig: "%s : REJECTING connection \"%s\" because pcid
					//  \"%s\" matches \"%s\"!" @0x7CFAE0 -> NapiFile_AppendLine("_inout.txt")
					//  @0x512865, gated on dword_B4C688].
					io::logf(io::LogLevel::kInfo,
							"%s : REJECTING connection \"%s\" because pcid \"%s\" matches \"%s\"!",
							stamp.c_str(), peer_addr_to_string(other.peer).c_str(), pcid.c_str(),
							other.account.pcid.c_str());
				}
				return 30;
			}
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

// [orig: Server_HandleBanPuntCommand @0x50B380 with senderPlayer 0 — authorized, and every
//  chat reply is gated on a sender, so none is sent. The line through
//  String_TokenizeQuotedToArray @0x616D60; fewer than one token is not handled. With two or
//  more tokens:
//  - BAN: with the list, `atol` of token 1 range-checked 0..capacity (inclusive), the slot by
//    index, active, not the local one, its net player's PCID nonempty -> LinkedList_AddEntry
//    with the slot name and BanList_SaveToFileWithHeader; handled either way.
//  - UNBAN: the same walk, then LinkedList_FindByName and LinkedList_RemoveAndFreeNode, saved.
//  Then, when the line was neither:
//  - PUNT (exactly two tokens): ALL -> every active slot not local, not a bot (+96483) and not
//    dropping (+96481) punted reason 33 "S.C:SPA"; else `atol` 0..capacity, that slot under the
//    same tests punted "S.C:SP#"; handled.
//  - BANDWIDTH (two tokens: g_EntitySendBudget = clamp(atol / 5, 100, 1600); one token: 600);
//    not handled, so the console's chain goes on.]
bool Server_HandleBanPuntCommand(NapiNPServerCtx &ctx, std::string_view line) {
	const std::vector<std::string> tokens = tokenize_quoted(line);
	if (tokens.empty()) return false;
	const int32_t capacity = static_cast<int32_t>(ctx.config.total_player_slot_capacity());
	if (tokens.size() > 1 && strutil::iequals(tokens[0], "BAN")) {
		if (!ctx.bans.pcids.has_value()) return true;
		const int32_t index = io::retail_atol(tokens[1].c_str());
		if (index < 0 || index > capacity) return true;
		NapiNPConnection *slot = slot_at(ctx, index);
		if (slot == nullptr || slot_is_local(*slot) || slot->account.pcid.empty()) return true;
		if (banlist::add(*ctx.bans.pcids, slot->account.pcid, slot->reply.player_name))
			Server_SavePcidBanList(ctx);
		return true;
	}
	if (tokens.size() > 1 && strutil::iequals(tokens[0], "UNBAN")) {
		if (!ctx.bans.pcids.has_value()) return true;
		const int32_t index = io::retail_atol(tokens[1].c_str());
		if (index < 0 || index > capacity) return true;
		NapiNPConnection *slot = slot_at(ctx, index);
		if (slot == nullptr || slot_is_local(*slot) || slot->account.pcid.empty()) return true;
		if (banlist::remove(*ctx.bans.pcids, slot->account.pcid)) Server_SavePcidBanList(ctx);
		return true;
	}
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
