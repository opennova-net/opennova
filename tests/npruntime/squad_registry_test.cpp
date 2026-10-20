// npruntime_squad_registry: the authority's NovaWorld squad registry (D-NET-NEW-L8-2) against
// retail's player add and disconnect, and its readers.
//   - a remote joiner whose account carries a squad files the squad's node and takes its id as
//     its key; as the squad's only active slot it sends S2C 0x6A {1, id, name, tag} on mask
//     0x1010 (every slot holder, itself included, its copy riding the spawn pump between the
//     settings updates and the 0x05) and S2C 0x32 {3, name} on mask 0x80 ahead of the join line;
//   - a second joiner of the squad overwrites the node's name and tag and sends neither;
//   - the map change's slot reset keeps the key and the node;
//   - the leave of a squad's second-to-last slot sends nothing; the last one's sends S2C 0x32
//     {4, name}, then S2C 0x6A {2, id}, and frees the node;
//   - a chat line carries the sender's squad tag after its name; channel 12 reaches the in-match
//     slots of the sender's squad key (0 on LAN matches 0) and team, never under the triplet
//     filter; the 0x56 row's tag is the node's;
//   - a session create starts with an empty registry.
// [orig: Server_PlayerAdd @0x51CBC0 - @0x51CDE0..0x51CEEF, @0x51D182..0x51D20E, the join line
//  @0x51D21E; CLinkedList_FindOrCreateByNetId @0x52B540; CSlotArray152_CountActiveByKey
//  @0x500660; NapiNPServer_SendFiltered @0x4C87E0 (the 0x1000 arm @0x4C89B5, 0x10 @0x4C88F6);
//  Server_HandlePlayerDisconnect @0x51B5C0 - @0x51B6D8..0x51B7FD; COverlayList_ReleaseNode
//  @0x52B100; Server_DisconnectAndResetAllPlayerSlots @0x516160; NapiNPServer_HandleChatMessage
//  @0x513760 - @0x513903..0x513930, @0x513EAE..0x513EE5; Server_BuildEndOfRoundScoreboard
//  @0x5090E8..0x509116; Server_ReleaseSessionTables @0x51B500]

#include <runtime/inmatch/map_change.h> // reset_slot_for_next_mission
#include <runtime/inmatch/napi_np_protocol.h>
#include <runtime/inmatch/server_chat.h>
#include <runtime/inmatch/server_spawn.h>
#include <runtime/inmatch/server_squad_registry.h>
#include <runtime/inmatch/udp_session_transport.h>

#include "host_test_setup.h"

#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_decode_session.h>
#include <net/npwire/ingame_message_id.h>
#include <runtime/world/entity.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

namespace inmatch = opennova::inmatch;
namespace w = opennova::world;
namespace ns = opennova::replication;

int failures = 0;
void expect(bool cond, const char *what) {
	if (cond) return;
	std::fprintf(stderr, "FAIL: %s\n", what);
	++failures;
}

std::vector<ns::Datagram> drain(ns::UdpSessionTransport &transport) {
	std::vector<ns::Datagram> out;
	ns::Datagram d;
	while (transport.pop_outbound(d)) out.push_back(d);
	return out;
}

std::size_t index_of(const std::vector<ns::Datagram> &out, uint8_t tag, std::size_t from = 0) {
	for (std::size_t i = from; i < out.size(); ++i)
		if (out[i].tag == tag) return i;
	return out.size();
}

std::size_t count_of(const std::vector<ns::Datagram> &out, uint8_t tag) {
	std::size_t n = 0;
	for (const ns::Datagram &d : out) n += d.tag == tag ? 1 : 0;
	return n;
}

bool roster_update(const ns::Datagram &d, opennova::ClanRosterUpdate &out) {
	return d.tag == opennova::s2c::CLAN_ROSTER &&
	       opennova::decode_clan_roster_update(d.body.data(), d.body.size(), out);
}

bool game_text(const ns::Datagram &d, opennova::FormattedGameText &out) {
	return d.tag == opennova::s2c::FORMATTED_GAME_TEXT &&
	       opennova::decode_formatted_game_text(d.body.data(), d.body.size(), out);
}

// A Serve Only host with one in-match LAN player (no squad) and room for three joiners. Heap-held:
// a World and a server context overflow the 1 MB test-main stack under MSVC.
struct SquadFixture {
	std::unique_ptr<w::World> world = std::make_unique<w::World>();
	std::unique_ptr<inmatch::NapiNPServerCtx> ctx = std::make_unique<inmatch::NapiNPServerCtx>();
	ns::UdpSessionTransport lan_transport{ns::UdpSessionTransport::Role::Host};
	ns::UdpSessionTransport transports[3] = {
			ns::UdpSessionTransport{ns::UdpSessionTransport::Role::Host},
			ns::UdpSessionTransport{ns::UdpSessionTransport::Role::Host},
			ns::UdpSessionTransport{ns::UdpSessionTransport::Role::Host}};
	uint32_t tick = 10;

	SquadFixture() {
		world->registry.configure_pool(0, 16);
		world->registry.configure_pool(3, 16);
		w::Entity start;
		start.kind = w::EntityKind::Marker;
		start.item_id = 6002;
		start.item_type_index = 1;
		start.has_item_def = true;
		world->registry.spawn(3, start);
		inmatch::GameConfig config;
		config.max_players = 5;
		inmatch::test::bring_up_host(*ctx, inmatch::ConnectionMode::HostOnly,
				inmatch::SocketMode::Socketless, /*host_key=*/0, nullptr, config);
		ctx->world = world.get();
		inmatch::Server_InitNewRoundState(*ctx);

		inmatch::NapiNPConnection &lan = ctx->np_protocol.connection_list.emplace_back();
		lan.type = inmatch::NapiNPConnection::kTypeServerSide;
		lan.peer = opennova::PeerAddr{0x0100007Fu, 30001};
		lan.connection_id = inmatch::kFirstJoinerDcb;
		lan.phase = inmatch::ConnectionPhase::InMatch;
		lan.slot_state = inmatch::PlayerSlotState::Joined;
		lan.burst.spawned = true;
		lan.reply.player_slot = 1;
		lan.reply.player_name = "Lan";
		lan.link.transport = &lan_transport;
		lan.link.mode = ns::TransportMode::Client;
	}

	inmatch::NapiNPConnection &lan() { return ctx->np_protocol.connection_list[0]; }

	// One admitted joiner waiting for the spawn pump; its account carries `squad_id`.
	inmatch::NapiNPConnection &queue_joiner(int index, const std::string &name, uint32_t squad_id,
			const std::string &squad_name, const std::string &squad_tag) {
		inmatch::NapiNPConnection &j = ctx->np_protocol.connection_list.emplace_back();
		j.type = inmatch::NapiNPConnection::kTypeServerSide;
		j.peer = opennova::PeerAddr{0x0100007Fu, static_cast<uint16_t>(30002 + index)};
		j.connection_id = inmatch::kFirstJoinerDcb + 1 + static_cast<uint32_t>(index);
		j.player_name = name;
		j.phase = inmatch::ConnectionPhase::Joined;
		j.admission_stage = inmatch::GameAdmissionStage::Complete;
		j.reply.admission_metadata_pushed = true;
		j.self_id_seen = true;
		j.link.transport = &transports[index];
		j.link.mode = ns::TransportMode::Client;
		j.account.squad_id = squad_id;
		j.account.squad_name = squad_name;
		j.account.squad_tag = squad_tag;
		return j;
	}

	inmatch::NapiNPConnection *by_peer(uint16_t port) {
		for (inmatch::NapiNPConnection &c : ctx->np_protocol.connection_list)
			if (c.peer.port == port) return &c;
		return nullptr;
	}
};

void run_two_joiners_of_one_squad_and_their_leave() {
	SquadFixture f;
	f.queue_joiner(0, "Able", 7, "Alpha Squad", "ALF");
	expect(inmatch::Server_ProcessPendingPlayerSpawns(*f.ctx, *f.world) == 1, "the pump adds Able");
	inmatch::NapiNPConnection *able = f.by_peer(30002);
	if (able == nullptr) return;
	expect(able->squad_key == 7, "Able's slot keys squad 7");
	expect(f.ctx->squad_registry.size() == 1 && f.ctx->squad_registry[0].squad_id == 7 &&
	               f.ctx->squad_registry[0].name == "Alpha Squad" &&
	               f.ctx->squad_registry[0].tag == "ALF",
	       "the add files squad 7's node with the account's name and tag");

	// The LAN player (state 6): the 0x6A {1}, then the squadron line, then the join line and
	// the 0x46.
	std::vector<ns::Datagram> lan = drain(f.lan_transport);
	const std::size_t roster = index_of(lan, opennova::s2c::CLAN_ROSTER);
	const std::size_t squadron = index_of(lan, opennova::s2c::FORMATTED_GAME_TEXT);
	const std::size_t joined = index_of(lan, opennova::s2c::FORMATTED_GAME_TEXT, squadron + 1);
	const std::size_t sync = index_of(lan, opennova::s2c::PLAYER_SYNC);
	expect(roster < squadron && squadron < joined && joined < sync && sync < lan.size(),
	       "the LAN slot hears the 0x6A, the squadron line, the join line, then the 0x46");
	opennova::ClanRosterUpdate update;
	expect(roster < lan.size() && roster_update(lan[roster], update) &&
	               update.action == opennova::kClanRosterAdd && update.account_id == 7 &&
	               update.name == "Alpha Squad" && update.tag == "ALF",
	       "the 0x6A is {1, 7, \"Alpha Squad\", \"ALF\"}");
	opennova::FormattedGameText text;
	expect(squadron < lan.size() && game_text(lan[squadron], text) &&
	               text.subtype == opennova::kGameTextSquadronEntered && text.text == "Alpha Squad",
	       "the squadron line is {3, \"Alpha Squad\"}");
	expect(joined < lan.size() && game_text(lan[joined], text) &&
	               text.subtype == opennova::kGameTextPlayerJoined,
	       "the join line follows it");

	// Able's own copy: none at the add, then between the settings updates and the 0x05.
	expect(drain(f.transports[0]).empty(), "Able's own 0x6A waits for the pump's metadata");
	(void)inmatch::tick_connections(*f.ctx, 16, f.tick++, /*respect_s2c_send_boundary=*/false,
			/*run_spawn_pump=*/false);
	std::vector<ns::Datagram> own = drain(f.transports[0]);
	const std::size_t reset = index_of(own, opennova::s2c::SYNC_TICK);
	const std::size_t own_roster = index_of(own, opennova::s2c::CLAN_ROSTER);
	const std::size_t start = index_of(own, opennova::s2c::GAME_START_SIGNAL);
	expect(reset < own_roster && own_roster < start && start < own.size() &&
	               own_roster == reset + 3 && count_of(own, opennova::s2c::CLAN_ROSTER) == 1,
	       "Able hears its 0x6A after the 0x03 and the two settings updates, ahead of the 0x05");
	expect(own_roster < own.size() && roster_update(own[own_roster], update) &&
	               update.action == opennova::kClanRosterAdd && update.account_id == 7,
	       "Able's copy is the same {1, 7}");

	// The second joiner of squad 7: the node takes its spellings; nobody hears a 0x6A or a
	// squadron line.
	f.queue_joiner(1, "Baker", 7, "Alpha Renamed", "ALF2");
	expect(inmatch::Server_ProcessPendingPlayerSpawns(*f.ctx, *f.world) == 1, "the pump adds Baker");
	inmatch::NapiNPConnection *baker = f.by_peer(30003);
	able = f.by_peer(30002);
	if (baker == nullptr || able == nullptr) return;
	expect(baker->squad_key == 7 && f.ctx->squad_registry.size() == 1 &&
	               f.ctx->squad_registry[0].name == "Alpha Renamed" &&
	               f.ctx->squad_registry[0].tag == "ALF2",
	       "the second joiner overwrites the node's name and tag");
	lan = drain(f.lan_transport);
	expect(count_of(lan, opennova::s2c::CLAN_ROSTER) == 0 &&
	               count_of(lan, opennova::s2c::FORMATTED_GAME_TEXT) == 1,
	       "the second joiner sends no 0x6A and no squadron line, only its join line");
	expect(drain(f.transports[0]).empty(), "Able, still loading, hears no second 0x6A");
	expect(baker->squad_roster_add_staged.empty(), "Baker stages no 0x6A");
	expect(inmatch::count_active_slots_by_squad_key(*f.ctx, 7) == 2 &&
	               inmatch::count_active_slots_by_squad_key(*f.ctx, 0) == 0,
	       "two active slots key squad 7; a key of 0 counts none");
	expect(inmatch::squad_registry_tag_for_slot(*f.ctx, baker->reply.player_slot) == "ALF2" &&
	               inmatch::squad_registry_tag_for_slot(*f.ctx, f.lan().reply.player_slot).empty(),
	       "the 0x56 row's tag is the node's; a LAN slot's is empty");

	// The map change keeps the slots' keys and the node.
	inmatch::reset_slot_for_next_mission(*baker);
	expect(baker->squad_key == 7 && f.ctx->squad_registry.size() == 1,
	       "the map change's slot reset keeps the key and the node");

	// Baker leaves: two counted, nothing sent, the node stays.
	(void)drain(f.lan_transport);
	expect(inmatch::destroy_connection(*f.ctx, opennova::PeerAddr{0x0100007Fu, 30003}, nullptr),
	       "Baker's teardown");
	lan = drain(f.lan_transport);
	expect(count_of(lan, opennova::s2c::CLAN_ROSTER) == 0 &&
	               f.ctx->squad_registry.size() == 1,
	       "the leave of a squad's second-to-last slot sends no 0x6A and keeps the node");
	opennova::FormattedGameText leave_text;
	bool squadron_left = false;
	for (const ns::Datagram &d : lan)
		if (game_text(d, leave_text) && leave_text.subtype == opennova::kGameTextSquadronLeft)
			squadron_left = true;
	expect(!squadron_left, "and no squadron-left line");

	// Able leaves: the squad's last slot sends {4, name}, then {2, id}, and the node goes.
	expect(inmatch::destroy_connection(*f.ctx, opennova::PeerAddr{0x0100007Fu, 30002}, nullptr),
	       "Able's teardown");
	lan = drain(f.lan_transport);
	std::size_t left = lan.size();
	for (std::size_t i = 0; i < lan.size() && left == lan.size(); ++i)
		if (game_text(lan[i], leave_text) && leave_text.subtype == opennova::kGameTextSquadronLeft)
			left = i;
	const std::size_t removed = index_of(lan, opennova::s2c::CLAN_ROSTER);
	expect(left < removed && removed < lan.size() && leave_text.text == "Alpha Renamed",
	       "the last slot's leave sends {4, \"Alpha Renamed\"} ahead of the 0x6A");
	expect(removed < lan.size() && roster_update(lan[removed], update) &&
	               update.action == opennova::kClanRosterRemove && update.account_id == 7 &&
	               lan[removed].body.size() == 5,
	       "the 0x6A is the 5-byte {2, 7}");
	expect(f.ctx->squad_registry.empty(), "the node is freed");
}

// A chat line from a squad member carries its tag; channel 12 reaches the slots of the
// sender's key and team (on LAN every key is 0), never under the triplet filter.
void run_chat_reads_the_squad_key() {
	SquadFixture f;
	auto body = [&](uint8_t team, float x) {
		w::Entity e;
		e.kind = w::EntityKind::Organic;
		e.team = team;
		e.position = {x, 0.0f, 0.0f};
		return f.world->registry.spawn(0, e);
	};
	// Three in-match remote slots: two on team 1 (one the sender), one on team 2.
	std::vector<inmatch::NapiNPConnection> &list = f.ctx->np_protocol.connection_list;
	list[0].link.owned_entity = body(1, 0.0f);
	inmatch::NapiNPConnection &mate = f.queue_joiner(0, "Mate", 0, "", "");
	mate.phase = inmatch::ConnectionPhase::InMatch;
	mate.burst.spawned = true;
	mate.reply.player_slot = 2;
	mate.reply.player_name = "Mate";
	mate.link.owned_entity = body(1, 1.0f);
	inmatch::NapiNPConnection &foe = f.queue_joiner(1, "Foe", 0, "", "");
	foe.phase = inmatch::ConnectionPhase::InMatch;
	foe.burst.spawned = true;
	foe.reply.player_slot = 3;
	foe.reply.player_name = "Foe";
	foe.link.owned_entity = body(2, 2.0f);
	inmatch::NapiNPConnection &sender = list[0];

	opennova::ChatUplink line;
	line.channel = 12;
	line.text = "squad up";
	(void)inmatch::Server_HandleChatMessage(*f.ctx, sender, line, 5000, *f.world);
	std::vector<ns::Datagram> to_mate = drain(f.transports[0]);
	std::vector<ns::Datagram> to_foe = drain(f.transports[1]);
	opennova::ChatBroadcast chat;
	expect(to_mate.size() == 1 && to_mate[0].tag == opennova::s2c::CHAT_BROADCAST &&
	               opennova::decode_chat_broadcast(to_mate[0].body.data(), to_mate[0].body.size(),
	                       chat) && chat.channel == 12 && chat.text == "Lan: squad up",
	       "channel 12 reaches a team mate of the same key (0 matches 0)");
	expect(to_foe.empty(), "and not the other team");

	// Under the triplet filter channel 12 reaches nobody, the sender included.
	line.text = "1 2 3";
	const std::vector<opennova::ProtocolMessage> own =
			inmatch::Server_HandleChatMessage(*f.ctx, sender, line, 7000, *f.world);
	expect(own.empty() && drain(f.transports[0]).empty(),
	       "the triplet filter keeps channel 12 from everyone");

	// A squad key: the line carries the node's tag, and channel 12 needs the same key.
	f.ctx->squad_registry.push_back({9, "Nine Squad", "NIN"});
	sender.squad_key = 9;
	line.channel = 1;
	line.text = "hello";
	(void)inmatch::Server_HandleChatMessage(*f.ctx, sender, line, 9000, *f.world);
	to_mate = drain(f.transports[0]);
	expect(to_mate.size() == 1 &&
	               opennova::decode_chat_broadcast(to_mate[0].body.data(), to_mate[0].body.size(),
	                       chat) && chat.text == "LanNIN: hello",
	       "the line carries the sender's squad tag after its name");
	(void)drain(f.transports[1]);
	line.channel = 12;
	(void)inmatch::Server_HandleChatMessage(*f.ctx, sender, line, 11000, *f.world);
	expect(drain(f.transports[0]).empty(), "channel 12 skips a team mate of another key");
}

// A session create clears the previous session's registry.
void run_session_create_clears_the_registry() {
	auto ctx = std::make_unique<inmatch::NapiNPServerCtx>();
	ctx->squad_registry.push_back({3, "Old", "OLD"});
	inmatch::test::bring_up_host(*ctx, inmatch::ConnectionMode::HostOnly,
			inmatch::SocketMode::Socketless);
	expect(ctx->squad_registry.empty(), "a session create starts with an empty registry");
}

} // namespace

int main() {
	run_two_joiners_of_one_squad_and_their_leave();
	run_chat_reads_the_squad_key();
	run_session_create_clears_the_registry();
	if (failures == 0) std::printf("npruntime_squad_registry: all passed\n");
	return failures == 0 ? 0 : 1;
}
