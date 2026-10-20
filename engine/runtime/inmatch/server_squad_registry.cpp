#include <runtime/inmatch/server_squad_registry.h>

#include <net/npwire/ingame_decode.h>         // ClanRosterUpdate, kClanRoster*
#include <net/npwire/ingame_decode_session.h> // FormattedGameText, kGameTextSquadron*
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>

#include <algorithm>
#include <vector>

namespace opennova::inmatch {
namespace {

// NapiNPServer_SendFiltered's mask 0x1010: the 0x1000 arm takes every
// connection whose player has a slot (active, not a bot) and reads none of the
// other bits but 0x10, which leaves out the listen host's own connection. No
// slot-state test: a squad's first slot hears its own 0x6A while it loads.
// [orig: NapiNPServer_SendFiltered @0x4C87E0 — the slot gate @0x4C889F, the
//  0x10 skip @0x4C88F6..0x4C88FA, the 0x1000 test @0x4C89B5..0x4C89B7]
void send_squad_roster(NapiNPServerCtx &ctx, const ClanRosterUpdate &update,
		const NapiNPConnection *skip) {
	const std::vector<uint8_t> body = encode_clan_roster_update(update);
	for (NapiNPConnection &c : ctx.np_protocol.connection_list) {
		if (&c == skip || c.link.transport == nullptr || !player_slot_active(c)) continue;
		if (c.link.mode == replication::TransportMode::Loopback) continue; // 0x10
		c.link.transport->host_send(s2c::CLAN_ROSTER, body); // class 1, userParam 0
	}
}

// The squadron lines on send mask 0x80 (slot states 6 and 7), reliable.
void send_squadron_text(NapiNPServerCtx &ctx, int8_t subtype, const std::string &name,
		const NapiNPConnection *skip) {
	FormattedGameText text;
	text.subtype = subtype;
	text.text = name;
	const std::vector<uint8_t> body = encode_formatted_game_text(text);
	for (NapiNPConnection &c : ctx.np_protocol.connection_list) {
		if (&c == skip || !active_player_recipient(c)) continue;
		c.link.transport->host_send(s2c::FORMATTED_GAME_TEXT, body);
	}
}

// Napi_CopyString(dst, src, n): at most n - 1 chars.
std::string node_copy(const std::string &value, std::size_t capacity) {
	return value.size() < capacity ? value : value.substr(0, capacity - 1);
}

} // namespace

const SquadRegistryNode *find_squad_registry_node(const NapiNPServerCtx &ctx, int32_t squad_id) {
	for (const SquadRegistryNode &node : ctx.squad_registry)
		if (node.squad_id == squad_id) return &node;
	return nullptr;
}

int32_t count_active_slots_by_squad_key(const NapiNPServerCtx &ctx, int32_t key) {
	if (key == 0) return 0;
	int32_t count = 0;
	for (const NapiNPConnection &c : ctx.np_protocol.connection_list)
		if (player_slot_active(c) && c.squad_key == key) ++count;
	return count;
}

void Server_FileSquadRegistryNode(NapiNPServerCtx &ctx, NapiNPConnection &conn) {
	conn.squad_key = 0; // @0x51CDE0
	const int32_t squad_id = static_cast<int32_t>(conn.account.squad_id); // join record +0x1A4
	if (squad_id == 0) return;
	// The upsert: an existing node takes the account's name and tag too; a new
	// one goes on the list's tail [orig: CLinkedList_FindOrCreateByNetId
	//  @0x52B540; COverlayList_FindOrAlloc @0x52B370, the tail link @0x52B3D7..0x52B401].
	const auto found = std::find_if(ctx.squad_registry.begin(), ctx.squad_registry.end(),
			[&](const SquadRegistryNode &n) { return n.squad_id == squad_id; });
	SquadRegistryNode *node = found != ctx.squad_registry.end() ? &*found : nullptr;
	if (node == nullptr) {
		node = &ctx.squad_registry.emplace_back();
		node->squad_id = squad_id;
	}
	node->name = node_copy(conn.account.squad_name, kClanRosterNameChars + 1); // +0x1A8 -> +16
	node->tag = node_copy(conn.account.squad_tag, kClanRosterTagChars + 1);    // +0x1E8 -> +81
	conn.squad_key = node->squad_id;
	if (count_active_slots_by_squad_key(ctx, conn.squad_key) != 1) return;
	ClanRosterUpdate update;
	update.action = kClanRosterAdd;
	update.account_id = static_cast<uint32_t>(node->squad_id);
	update.name = node->name;
	update.tag = node->tag;
	send_squad_roster(ctx, update, &conn);
	// The added slot is a recipient too: its copy rides the spawn pump's
	// metadata (NapiNPConnection::squad_roster_add_staged).
	if (conn.link.mode != replication::TransportMode::Loopback)
		conn.squad_roster_add_staged = encode_clan_roster_update(update);
}

void Server_AnnounceSquadronEntered(NapiNPServerCtx &ctx, const NapiNPConnection &conn) {
	if (conn.squad_key == 0 || count_active_slots_by_squad_key(ctx, conn.squad_key) != 1) return;
	// No node: an empty body, nothing sent [orig: @0x51D1AB, @0x51D1F6..0x51D1FB].
	const SquadRegistryNode *node = find_squad_registry_node(ctx, conn.squad_key);
	if (node == nullptr) return;
	send_squadron_text(ctx, kGameTextSquadronEntered, node->name, &conn);
}

void Server_ReleaseSquadRegistryNode(NapiNPServerCtx &ctx, NapiNPConnection &leaver) {
	if (leaver.squad_key == 0) return;
	// The leaver is still active, so its squad's last slot counts 1. Its own
	// connection would take the sends too, after its disconnect burst, into a
	// queue its destroy drops; the port leaves it out.
	// [orig: CNapiNPConnection_TeardownActiveConnection @0x6253C0 — the burst
	//  @0x6253EF..0x625424 ahead of the callback @0x625438]
	if (count_active_slots_by_squad_key(ctx, leaver.squad_key) <= 1) {
		if (const SquadRegistryNode *node = find_squad_registry_node(ctx, leaver.squad_key))
			send_squadron_text(ctx, kGameTextSquadronLeft, node->name, &leaver);
	}
	if (count_active_slots_by_squad_key(ctx, leaver.squad_key) <= 1) {
		const auto node = std::find_if(ctx.squad_registry.begin(), ctx.squad_registry.end(),
				[&](const SquadRegistryNode &n) { return n.squad_id == leaver.squad_key; });
		if (node != ctx.squad_registry.end()) {
			ClanRosterUpdate update;
			update.action = kClanRosterRemove;
			update.account_id = static_cast<uint32_t>(node->squad_id);
			send_squad_roster(ctx, update, &leaver);
			ctx.squad_registry.erase(node);
		}
	}
	leaver.squad_key = 0;
}

std::string squad_registry_tag_for_slot(const NapiNPServerCtx &ctx, uint8_t player_slot) {
	for (const NapiNPConnection &c : ctx.np_protocol.connection_list) {
		if (!player_slot_active(c) || c.reply.player_slot != player_slot) continue;
		if (c.squad_key == 0) return {};
		const SquadRegistryNode *node = find_squad_registry_node(ctx, c.squad_key);
		return node != nullptr ? node->tag : std::string();
	}
	return {};
}

} // namespace opennova::inmatch
