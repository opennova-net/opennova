// The host dispatch's remaining C2S cases (D-NET-218): the 0x34 keepalive's high-water store,
// the 0x4E squad registry walk (S2C 0x6A action 3 to the requester, nothing past the last node
// or from an empty registry) and the 0x24 ban and punt line (inert from a remote slot; the
// local slot's BAN / UNBAN answered in channel-3 S2C 0x14 lines).
// [orig: NapiNPServerMsg_UpdateAck @0x5024B0; NapiNPServerMsg_HandleSquadRegistryWalk
//  @0x511210 -> CLinkedList_FindNextByNetId @0x52B190; NapiNPServerMsg_0x024 @0x514DC0 ->
//  Server_HandleBanPuntCommand @0x50B380 -> Server_SendChatMessageToPlayer @0x508050]

#include <formats/banlist/pcid_ban_list.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_decode_session.h> // decode_chat_broadcast
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/protocol_message.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_message_dispatch.h>
#include <runtime/inmatch/server_session.h> // set_connection_mode
#include <runtime/inmatch/server_tick.h>    // Server_RerollPlayerTickSeed / Server_DisarmPlayerTickSeed
#include <runtime/inmatch/udp_session_transport.h>
#include <base/io/le.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace opennova;

namespace {

int g_failures = 0;

void expect(bool cond, const char *what) {
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++g_failures;
	}
}

std::vector<uint8_t> u32_body(uint32_t value) {
	std::vector<uint8_t> body;
	io::append_u32_le(body, value);
	return body;
}

inmatch::NapiNPConnection slot(uint8_t number, const std::string &name, const std::string &pcid,
		replication::ISessionTransport *transport, bool local = false) {
	inmatch::NapiNPConnection c;
	c.type = local ? inmatch::NapiNPConnection::kTypeClientSide : inmatch::NapiNPConnection::kTypeServerSide;
	c.phase = inmatch::ConnectionPhase::InMatch;
	c.link.mode = local ? replication::TransportMode::Loopback : replication::TransportMode::Client;
	c.link.transport = transport;
	c.admission_stage = inmatch::GameAdmissionStage::Complete;
	c.reply.player_slot = number;
	c.reply.player_name = name;
	c.account.pcid = pcid;
	return c;
}

std::vector<ProtocolMessage> dispatch(inmatch::NapiNPServerCtx &ctx, size_t from, uint8_t tag,
		std::vector<uint8_t> body) {
	inmatch::ServerDispatchInputs inputs;
	inputs.server_ctx = &ctx;
	return inmatch::dispatch_session_replies(ctx.config, ctx.np_protocol.connection_list[from],
			{make_protocol_message(tag, std::move(body))}, 5, ctx.np_protocol.connection_list,
			nullptr, inputs);
}

std::vector<uint8_t> line_body(const std::string &line) {
	std::vector<uint8_t> body(line.begin(), line.end());
	body.push_back(0);
	return body;
}

// One S2C 0x14 of the replies (or none), as [channel][slot][text].
bool chat_reply(const std::vector<ProtocolMessage> &replies, ChatBroadcast &out) {
	for (const ProtocolMessage &m : replies) {
		if (m.tag != s2c::CHAT_BROADCAST) continue;
		return decode_chat_broadcast(m.payload.data(), m.payload.size(), out) &&
		       m.retention_flushes == 0x136;
	}
	return false;
}

// The seed's roll sets the high-water to the seed; a 0x34 at or past both the seed and the
// high-water raises it, signed; a short body reads 0; the disarm clears it and stops the store.
void test_keepalive_high_water() {
	inmatch::NapiNPServerCtx ctx;
	inmatch::set_connection_mode(ctx, inmatch::ConnectionMode::HostOnly);
	ctx.np_protocol.connection_list.push_back(slot(1, "Alpha", "", nullptr));
	inmatch::NapiNPConnection &conn = ctx.np_protocol.connection_list[0];
	(void)inmatch::Server_RerollPlayerTickSeed(conn, 1u);
	const uint32_t seed = conn.tick_seed;
	expect(conn.keepalive_ack_high_water == seed, "the roll sets the high-water to the seed (@0x5101DD)");
	expect(dispatch(ctx, 0, c2s::KEEPALIVE, u32_body(seed - 1)).empty(), "the 0x34 draws no reply");
	expect(conn.keepalive_ack_high_water == seed, "below the seed: no store");
	dispatch(ctx, 0, c2s::KEEPALIVE, u32_body(seed + 29760));
	expect(conn.keepalive_ack_high_water == seed + 29760, "at or past both: the store (@0x502503)");
	dispatch(ctx, 0, c2s::KEEPALIVE, u32_body(seed + 100));
	expect(conn.keepalive_ack_high_water == seed + 29760, "below the high-water: no store");
	dispatch(ctx, 0, c2s::KEEPALIVE, u32_body(0x80000000u));
	expect(conn.keepalive_ack_high_water == seed + 29760, "a negative value is below the seed (signed jl)");
	dispatch(ctx, 0, c2s::KEEPALIVE, {0xFF, 0xFF, 0xFF});
	expect(conn.keepalive_ack_high_water == seed + 29760, "a short body reads 0");
	(void)inmatch::Server_DisarmPlayerTickSeed(conn, 7);
	expect(conn.keepalive_ack_high_water == 0, "the disarm clears it (@0x510244)");
	dispatch(ctx, 0, c2s::KEEPALIVE, u32_body(seed + 60000));
	expect(conn.keepalive_ack_high_water == 0, "out of the tick mode: no store (@0x5024EA)");
}

// The walk: the smallest id strictly past the value (signed), as 0x6A action 3 with the node's
// name and tag; nothing past the last node or from an empty registry (a LAN host's).
void test_squad_registry_walk() {
	inmatch::NapiNPServerCtx ctx;
	inmatch::set_connection_mode(ctx, inmatch::ConnectionMode::HostOnly);
	ctx.np_protocol.connection_list.push_back(slot(1, "Alpha", "", nullptr));
	expect(dispatch(ctx, 0, c2s::GAME_START_ACK, u32_body(0)).empty(),
			"an empty registry answers the {0} kick with nothing (the LAN captures' silence)");
	ctx.squad_registry.push_back({905, "Ninth", "N9"});
	ctx.squad_registry.push_back({12, "Twelfth", "T12"});
	ctx.squad_registry.push_back({300, "Third", "T3"});
	const auto walk = [&](std::vector<uint8_t> body, ClanRosterUpdate &out) {
		const std::vector<ProtocolMessage> replies = dispatch(ctx, 0, c2s::GAME_START_ACK, std::move(body));
		if (replies.size() != 1 || replies[0].tag != s2c::CLAN_ROSTER) return false;
		return decode_clan_roster_update(replies[0].payload.data(), replies[0].payload.size(), out);
	};
	ClanRosterUpdate reply;
	expect(walk(u32_body(0), reply) && reply.action == kClanRosterWalkReply &&
	               reply.account_id == 12 && reply.name == "Twelfth" && reply.tag == "T12",
	       "{0}: the smallest id, action 3 with its name and tag");
	expect(walk(u32_body(12), reply) && reply.account_id == 300, "strictly past 12: 300");
	expect(walk(u32_body(300), reply) && reply.account_id == 905, "strictly past 300: 905");
	expect(dispatch(ctx, 0, c2s::GAME_START_ACK, u32_body(905)).empty(), "past the last node: nothing");
	expect(walk({0x05, 0x00}, reply) && reply.account_id == 12, "a short body reads 0");
	expect(walk(u32_body(0xFFFFFFFFu), reply) && reply.account_id == 12, "the compare is signed: -1 walks from the start");
}

// A remote slot's 0x24 is inert: BAN / UNBAN handled with nothing, PUNT and BANDWIDTH refused.
// The local slot's BAN / UNBAN answer it in channel-3 0x14 lines, and the ban itself tells the
// banned slot.
void test_ban_punt_command() {
	inmatch::NapiNPServerCtx ctx;
	inmatch::set_connection_mode(ctx, inmatch::ConnectionMode::HostOnly);
	ctx.is_in_session = 1;
	ctx.config.max_players = 8;
	ctx.bans.pcids = banlist::PcidBanList{};
	replication::UdpSessionTransport alpha_link(replication::UdpSessionTransport::Role::Host);
	replication::UdpSessionTransport bravo_link(replication::UdpSessionTransport::Role::Host);
	ctx.np_protocol.connection_list.push_back(slot(0, "Host", "H-PCID", nullptr, /*local=*/true));
	ctx.np_protocol.connection_list.push_back(slot(1, "Alpha", "A-PC-ID0001", &alpha_link));
	ctx.np_protocol.connection_list.push_back(slot(2, "Bravo", "", &bravo_link));
	const uint32_t capacity = ctx.config.total_player_slot_capacity();
	const uint32_t budget = ctx.config.entity_send_budget;

	// Remote: nothing changes and nothing is sent.
	expect(dispatch(ctx, 2, c2s::BAN_PUNT_COMMAND, line_body("BAN 1")).empty() &&
	               ctx.bans.pcids->entries.empty(),
	       "a remote BAN does nothing (@0x50B43A)");
	dispatch(ctx, 2, c2s::BAN_PUNT_COMMAND, line_body("PUNT 1"));
	dispatch(ctx, 2, c2s::BAN_PUNT_COMMAND, line_body("BANDWIDTH 1000"));
	replication::Datagram staged;
	expect(!alpha_link.pop_outbound(staged) && !ctx.np_protocol.connection_list[1].host_disconnect_sent,
	       "a remote PUNT punts nobody (@0x50B845)");
	expect(ctx.config.entity_send_budget == budget, "a remote BANDWIDTH sets nothing");

	// Local: each refusal answers the sender.
	ChatBroadcast chat;
	expect(chat_reply(dispatch(ctx, 0, c2s::BAN_PUNT_COMMAND, line_body("BAN 99")), chat) &&
	               chat.channel == 3 && chat.sender_slot == 0 && chat.text == "Player number 99 out of range.",
	       "BAN out of range (@0x50B5F3), [3][the recipient's own slot]");
	expect(chat_reply(dispatch(ctx, 0, c2s::BAN_PUNT_COMMAND, line_body("BAN " + std::to_string(capacity))), chat) &&
	               chat.text == "Player #" + std::to_string(capacity) + " doesn't exist.",
	       "BAN of the capacity index: no row (@0x50B480)");
	expect(chat_reply(dispatch(ctx, 0, c2s::BAN_PUNT_COMMAND, line_body("BAN 5")), chat) &&
	               chat.text == "Player #5 not used.",
	       "BAN of a free row (@0x50B493)");
	expect(chat_reply(dispatch(ctx, 0, c2s::BAN_PUNT_COMMAND, line_body("BAN 0")), chat) &&
	               chat.text == "Cannot BAN the host player.",
	       "BAN of the host (@0x50B4CE)");
	expect(chat_reply(dispatch(ctx, 0, c2s::BAN_PUNT_COMMAND, line_body("BAN 2")), chat) &&
	               chat.text == "Player #2 \"Bravo\" doesn't have a PCID.",
	       "BAN of a slot with no PCID (@0x50B557)");
	expect(chat_reply(dispatch(ctx, 0, c2s::BAN_PUNT_COMMAND, line_body("UNBAN 1")), chat) &&
	               chat.text == "Player #1 \"Alpha\" with PCID of \"A-PC-ID0001\" doesn't appear in your banned list.",
	       "UNBAN of an unlisted slot (@0x50B7F0)");

	// The ban: listed, and the banned slot told, not the sender.
	expect(dispatch(ctx, 0, c2s::BAN_PUNT_COMMAND, line_body("BAN 1")).empty() &&
	               ctx.bans.pcids->entries.size() == 1 && ctx.bans.pcids->entries[0].pcid == "A-PC-ID0001",
	       "the local BAN lists the PCID, nothing back to the sender");
	bool told = false;
	while (alpha_link.pop_outbound(staged)) {
		if (staged.tag == s2c::CHAT_BROADCAST &&
				decode_chat_broadcast(staged.body.data(), staged.body.size(), chat))
			told = chat.channel == 3 && chat.sender_slot == 1 && chat.text == "Banned Player #1 \"Alpha\".";
	}
	expect(told, "\"Banned Player #1 \\\"Alpha\\\".\" to the banned slot (@0x50B5BB..0x50B5CB)");
	expect(chat_reply(dispatch(ctx, 0, c2s::BAN_PUNT_COMMAND, line_body("UNBAN 1")), chat) &&
	               chat.text == "Unbanned Player #1 \"Alpha\"." && ctx.bans.pcids->entries.empty(),
	       "the local UNBAN (@0x50B7B7)");
	expect(chat_reply(dispatch(ctx, 0, c2s::BAN_PUNT_COMMAND, line_body("UNBAN 77")), chat) &&
	               chat.text == "Player #77 out of range.",
	       "UNBAN out of range keeps its own spelling (@0x50B824)");

	// The local PUNT punts without a line.
	expect(dispatch(ctx, 0, c2s::BAN_PUNT_COMMAND, line_body("PUNT 1")).empty() &&
	               ctx.np_protocol.connection_list[1].host_disconnect_sent,
	       "the local PUNT punts");
}

} // namespace

int main() {
	test_keepalive_high_water();
	test_squad_registry_walk();
	test_ban_punt_command();
	if (g_failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("dispatch_remainder: ok\n");
	return 0;
}
