// The host side of the command map's squad and waypoint C2S (D-HUD-19): the
// relays of the waypoint share / delete, the squad-chain link and its
// rejections, the order lines, the fireteam assignment, the recruit, the go
// code, the break-up of a squad and the punt tally — each staged on the
// recipients' own transports exactly as the retail send filters pick them.
// [orig: NapiNPServerMsg_HandleChatOrWhisper @0x514850 (0x17),
//  NapiNPServerMsg_0x04F @0x514a40, Server_HandleEntitySync @0x510990 (0x43),
//  NapiNPServerMsg_HandleChatBroadcast @0x510ae0 (0x44),
//  NapiNPServerMsg_0x045_HandleTeamAssignment @0x510c00,
//  NapiNPServerMsg_HandleVoteKick @0x510d20 (0x46),
//  NapiNPServer_BroadcastPlayerProfileUpdate @0x510dc0 (0x4B),
//  Server_SendPlayerStateAndSquad @0x518b40, Server_ProcessVoteKickResults
//  @0x511400]

#include "npruntime/conn_fixture.h"

#include <net/npwire/ingame_message_id.h>
#include <net/npwire/squad_messages.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_squad.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <deque>
#include <iterator>
#include <memory>
#include <vector>
#include "../common/title_fixture.h"

using namespace opennova;

namespace {

int failures = 0;
#define CHECK(c) \
	do { \
		if (!(c)) { \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); \
			++failures; \
		} \
	} while (0)

// Three in-match players: slots 0 and 1 on team 1, slot 2 on team 2, each
// driving pool-0 entity slot + 4.
struct Host {
	inmatch::NapiNPServerCtx ctx;
	bool ctx_jo = test_titles::jo_config(ctx.config);
	std::deque<replication::LoopbackChannel> wires;
	world::World world;
	bool world_jo = test_titles::jo_world(world);

	Host() {
		ctx.is_authority = 1;
		ctx.config.max_players = 8;
		const uint8_t teams[3] = {1, 1, 2};
		for (uint8_t slot = 0; slot < 3; ++slot) {
			wires.emplace_back();
			inmatch::NapiNPConnection c = conn_fixture::make_conn(10u + slot, 1, &wires.back(),
					replication::TransportMode::Loopback, world::EntityHandle::make(0, slot + 4),
					true);
			c.reply.player_slot = slot;
			c.assigned_team_valid = true;
			c.assigned_team = teams[slot];
			ctx.np_protocol.connection_list.push_back(c);
		}
	}
	inmatch::NapiNPConnection &conn(int slot) {
		auto it = ctx.np_protocol.connection_list.begin();
		std::advance(it, slot);
		return *it;
	}
	// Every S2C staged on slot `slot`'s wire since the last drain.
	std::vector<replication::Datagram> drain(int slot) {
		std::vector<replication::Datagram> out;
		replication::Datagram d;
		while (wires[static_cast<size_t>(slot)].client_recv(d)) out.push_back(d);
		return out;
	}
	void drain_all() {
		for (int i = 0; i < 3; ++i) (void)drain(i);
	}
	bool handle(int slot, uint8_t tag, const std::vector<uint8_t> &body) {
		return inmatch::Server_HandleSquadMessage(ctx, conn(slot), tag, body, world);
	}
};

void test_squad_link() {
	Host h;
	// Slot 1 joins slot 0: the link, then S2C 0x71 [0][1] to team 1 alone.
	CHECK(h.handle(1, c2s::SQUAD_JOIN_REQUEST, encode_squad_join_request(0)));
	CHECK(h.conn(1).squad_leader == 0);
	for (int slot : {0, 1}) {
		const auto got = h.drain(slot);
		CHECK(got.size() == 1 && got[0].tag == s2c::SQUAD_JOIN);
		if (got.size() == 1) {
			const SquadJoin join = decode_squad_join(got[0].body.data(), got[0].body.size());
			CHECK(join.leader == 0 && join.member == 1);
		}
	}
	CHECK(h.drain(2).empty());
	// A cycle (0 under 1, whose leader is 0) and a cross-team link are
	// dropped silently.
	CHECK(h.handle(0, c2s::SQUAD_JOIN_REQUEST, encode_squad_join_request(1)));
	CHECK(h.conn(0).squad_leader == 0xFF);
	CHECK(h.handle(2, c2s::SQUAD_JOIN_REQUEST, encode_squad_join_request(0)));
	CHECK(h.conn(2).squad_leader == 0xFF);
	for (int slot = 0; slot < 3; ++slot) CHECK(h.drain(slot).empty());
	// A spectator sends nothing through.
	h.conn(2).link.spectator = true;
	CHECK(h.handle(2, c2s::SQUAD_JOIN_REQUEST, encode_squad_join_request(0xFF)));
	CHECK(h.drain(2).empty());
	// Not a squad tag.
	CHECK(!h.handle(0, c2s::WAYPOINT_SHARE + 1, {}));
}

void test_waypoint_relays() {
	Host h;
	h.conn(1).squad_leader = 0;
	WaypointShare share;
	share.target = 0xFF;
	share.name = "HQ";
	share.x = 100 << 16;
	share.y = -(50 << 16);
	share.z = 7 << 16;
	// 0xFF: every in-game slot slot 0 leads, the owner its pool-0 index.
	CHECK(h.handle(0, c2s::WAYPOINT_SHARE, encode_waypoint_share(share)));
	CHECK(h.drain(0).empty() && h.drain(2).empty());
	auto got = h.drain(1);
	CHECK(got.size() == 1 && got[0].tag == s2c::WAYPOINT_CREATE);
	if (got.size() == 1) {
		// The client's decode skips the z dword; the relay still carries it
		// ("HQ\0" + x + y, then z at byte 11).
		const std::vector<uint8_t> &b = got[0].body;
		const WaypointCreate create = decode_waypoint_create(b.data(), b.size());
		CHECK(create.name == "HQ" && create.x == share.x && create.y == share.y &&
				create.owner_index == 4);
		CHECK(b.size() == 16 && static_cast<int32_t>(b[11] | (b[12] << 8) | (b[13] << 16) |
				(uint32_t(b[14]) << 24)) == share.z);
	}
	// A named target on the sender's team; the other team's slot is refused.
	share.target = 1;
	h.conn(1).squad_leader = 0xFF;
	CHECK(h.handle(0, c2s::WAYPOINT_SHARE, encode_waypoint_share(share)));
	CHECK(h.drain(1).size() == 1);
	share.target = 2;
	CHECK(h.handle(0, c2s::WAYPOINT_SHARE, encode_waypoint_share(share)));
	CHECK(h.drain(2).empty());
	// The delete's handle reaches the led slots as S2C 0x7C.
	h.conn(1).squad_leader = 0;
	CHECK(h.handle(0, c2s::WAYPOINT_DELETE, encode_entity_handle16(0x4003)));
	got = h.drain(1);
	CHECK(got.size() == 1 && got[0].tag == s2c::DESTROY_ENTITY &&
			decode_entity_handle16(got[0].body.data(), got[0].body.size()) == 0x4003);
}

void test_orders_fireteams_recruit_and_go_codes() {
	Host h;
	h.conn(1).squad_leader = 0;
	// 0x44: the line to each listed slot, whatever its team.
	SquadOrderRequest order;
	order.kind = 1;
	order.text = "Hold-Alpha";
	order.targets = {1, 2, 200};
	CHECK(h.handle(0, c2s::SQUAD_ORDER_REQUEST, encode_squad_order_request(order)));
	for (int slot : {1, 2}) {
		const auto got = h.drain(slot);
		CHECK(got.size() == 1 && got[0].tag == s2c::SQUAD_ORDER);
		if (got.size() == 1) {
			const SquadOrder line = decode_squad_order(got[0].body.data(), got[0].body.size());
			CHECK(line.kind == 1 && line.text == "Hold-Alpha");
		}
	}
	// 0x45: the fireteam stored, S2C 0x73 to the member's team.
	FireteamAssign assign;
	assign.fireteam = 2;
	assign.members = {1, 99};
	CHECK(h.handle(0, c2s::FIRETEAM_ASSIGN, encode_fireteam_assign(assign)));
	CHECK(h.conn(1).fireteam == 2);
	auto got = h.drain(0);
	CHECK(got.size() == 1 && got[0].tag == s2c::FIRETEAM_SET);
	CHECK(h.drain(2).empty());
	(void)h.drain(1);
	// 0x46: S2C 0x74 [recruiter] to the target alone.
	SquadRecruit recruit;
	recruit.recruiter = 0;
	recruit.target = 2;
	CHECK(h.handle(0, c2s::SQUAD_RECRUIT, encode_squad_recruit(recruit)));
	got = h.drain(2);
	CHECK(got.size() == 1 && got[0].tag == s2c::SQUAD_RECRUITED &&
			decode_squad_recruited(got[0].body.data(), got[0].body.size()) == 0);
	// 0x4B: S2C 0x78 to every slot whose leader is the body's leader byte.
	GoCode go;
	go.leader = 0;
	go.code = 3;
	CHECK(h.handle(0, c2s::GO_CODE, encode_go_code(go)));
	got = h.drain(1);
	CHECK(got.size() == 1 && got[0].tag == s2c::GO_CODE);
	CHECK(h.drain(0).empty() && h.drain(2).empty());
}

void test_squad_breakup() {
	Host h;
	h.conn(1).squad_leader = 0;
	h.conn(1).fireteam = 3;
	inmatch::Server_DissolveSquadOf(h.ctx, h.conn(0));
	CHECK(h.conn(0).squad_leader == 0xFF && h.conn(1).squad_leader == 0xFF);
	CHECK(h.conn(1).fireteam == 0);
	// Slot 0: its own 0x71, its two cleared order lines, then slot 1's 0x71.
	auto got = h.drain(0);
	CHECK(got.size() == 4);
	if (got.size() == 4) {
		CHECK(got[0].tag == s2c::SQUAD_JOIN && got[1].tag == s2c::SQUAD_ORDER &&
				got[2].tag == s2c::SQUAD_ORDER && got[3].tag == s2c::SQUAD_JOIN);
		const SquadJoin join = decode_squad_join(got[3].body.data(), got[3].body.size());
		CHECK(join.leader == 0xFF && join.member == 1);
	}
	CHECK(h.drain(2).empty());
}

void test_punt_vote() {
	Host h;
	// No standing votes: a fresh slot's byte is the no-vote 0xFF.
	for (int slot = 0; slot < 3; ++slot) CHECK(h.conn(slot).punt_vote == 0xFF);
	// Voting off (the default): the vote is not even stored.
	CHECK(h.handle(0, c2s::PUNT_VOTE, encode_punt_vote(2)));
	CHECK(h.conn(0).punt_vote == 0xFF);
	h.ctx.config.voting_enabled = true;
	h.ctx.config.voting_min_players = 3;
	h.ctx.config.voting_percent = 0.66f;
	// Three active: the threshold is (int)(3 x 0.66 + 0.5) = 2 votes.
	CHECK(h.handle(0, c2s::PUNT_VOTE, encode_punt_vote(2)));
	CHECK(h.conn(0).punt_vote == 2 && !h.conn(2).host_disconnect_sent);
	CHECK(h.handle(1, c2s::PUNT_VOTE, encode_punt_vote(2)));
	CHECK(h.conn(2).host_disconnect_sent);
	CHECK(h.conn(0).punt_vote == 0xFF && h.conn(1).punt_vote == 0xFF);
}

// Retail's fresh punt byte is 0, which names row 0, always its own local row,
// and the tally skips a vote for the local row; here slot 0 is a joiner's,
// so a non-voter's byte must count nothing. One vote below the threshold
// punts nobody (a 0 seed would hand slot 0 the two silent slots' votes).
// [orig: Server_PlayerAdd @0x51cd06; Server_InitNewRoundState @0x51ca35;
//  Server_ProcessVoteKickResults @0x5114dc]
void test_punt_default_counts_nothing() {
	Host h;
	h.ctx.config.voting_enabled = true;
	h.ctx.config.voting_min_players = 3;
	h.ctx.config.voting_percent = 0.66f; // threshold 2
	CHECK(h.handle(1, c2s::PUNT_VOTE, encode_punt_vote(2)));
	for (int slot = 0; slot < 3; ++slot) CHECK(!h.conn(slot).host_disconnect_sent);
	CHECK(h.conn(0).punt_vote == 0xFF && h.conn(1).punt_vote == 2 &&
			h.conn(2).punt_vote == 0xFF);
}

// A stored target with no row (the 0x3F handler stores any byte and runs the
// tally only for a real target) counts nothing: retail reads a NULL row there
// (a host crash) and the 251-dword tally never grows past the slot rows.
// [orig: NapiNPServerMsg_VoteKick @0x518f53..0x518f69;
//  Server_ProcessVoteKickResults @0x5114c2..0x5114dc, @0x51154b]
void test_punt_target_without_a_row() {
	for (const uint8_t stray : {uint8_t{9}, uint8_t{254}}) {
		Host h;
		h.ctx.config.voting_enabled = true;
		h.ctx.config.voting_min_players = 3;
		h.ctx.config.voting_percent = 0.2f; // (int)(3 x 0.2 + 0.5) = 1 vote
		// The stray target is stored; no row, so no tally runs.
		CHECK(h.handle(1, c2s::PUNT_VOTE, encode_punt_vote(stray)));
		CHECK(h.conn(1).punt_vote == stray);
		// A real vote runs the tally: slot 2 goes, the stray vote counted
		// nothing, so it is not a vote for a punted target and stays.
		CHECK(h.handle(0, c2s::PUNT_VOTE, encode_punt_vote(2)));
		CHECK(h.conn(2).host_disconnect_sent && !h.conn(1).host_disconnect_sent);
		CHECK(h.conn(1).punt_vote == stray && h.conn(0).punt_vote == 0xFF);
	}
}

// The chain walk reads an empty row's leader as 0, so it steps to row 0. With
// row 0 empty (a port host with no local row; retail's row 0 is always the
// host's own) a join naming any empty row walks 0 -> 0 forever, and a stale
// link through an empty row (a member past state 6 keeps its leader byte, see
// the break-up) closes the same spin through an occupied row 0, as it does in
// retail (a host hang). The port rejects the link once the walk outlasts the
// slot rows.
// [orig: NetPacket_WritePlayerChainLink @0x510718..0x510756;
//  Server_HandlePlayerDisconnect @0x51b87d (the row memset)]
void test_squad_link_walk_ends() {
	{
		Host h;
		h.conn(0).reply.player_slot = 5; // row 0 empty
		CHECK(h.handle(1, c2s::SQUAD_JOIN_REQUEST, encode_squad_join_request(3)));
		CHECK(h.conn(1).squad_leader == 0xFF);
		for (int slot = 0; slot < 3; ++slot) CHECK(h.drain(slot).empty());
	}
	{
		Host h;
		h.conn(0).squad_leader = 3; // a stale link to the empty row 3
		CHECK(h.handle(1, c2s::SQUAD_JOIN_REQUEST, encode_squad_join_request(0)));
		CHECK(h.conn(1).squad_leader == 0xFF);
		for (int slot = 0; slot < 3; ++slot) CHECK(h.drain(slot).empty());
		// A chain that ends still links: row 0's link cleared, 1 joins 0.
		h.conn(0).squad_leader = 0xFF;
		CHECK(h.handle(1, c2s::SQUAD_JOIN_REQUEST, encode_squad_join_request(0)));
		CHECK(h.conn(1).squad_leader == 0);
	}
}

// The break-up resets the members in slot state 6 alone: a member still
// loading, or past the round end (state 7), keeps its leader byte.
// [orig: Server_SendPlayerStateAndSquad @0x518c3f..0x518c70;
//  Server_ProcessRoundEnd @0x51685e]
void test_squad_breakup_state_6_members() {
	{
		Host h;
		h.conn(1).squad_leader = 0;
		h.conn(1).burst.spawned = false; // loading
		inmatch::Server_DissolveSquadOf(h.ctx, h.conn(0));
		CHECK(h.conn(0).squad_leader == 0xFF && h.conn(1).squad_leader == 0);
	}
	{
		Host h;
		h.ctx.world = &h.world;
		h.conn(1).squad_leader = 0;
		h.conn(1).fireteam = 2;
		h.world.process_round_end(1);
		CHECK(h.world.match.outcome().ended);
		inmatch::Server_DissolveSquadOf(h.ctx, h.conn(0));
		CHECK(h.conn(0).squad_leader == 0xFF);
		CHECK(h.conn(1).squad_leader == 0 && h.conn(1).fireteam == 2);
		// Only slot 0's own 0x71 [0xFF][0] and its two order lines go out.
		CHECK(h.drain(0).size() == 3 && h.drain(1).size() == 1);
	}
}

} // namespace

int main() {
	test_squad_link();
	test_waypoint_relays();
	test_orders_fireteams_recruit_and_go_codes();
	test_squad_breakup();
	test_punt_vote();
	test_punt_default_counts_nothing();
	test_punt_target_without_a_row();
	test_squad_link_walk_ends();
	test_squad_breakup_state_6_members();
	if (failures == 0) std::printf("squad_relay: all checks passed\n");
	return failures == 0 ? 0 : 1;
}
