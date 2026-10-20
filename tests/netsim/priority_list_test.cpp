// D-NET-530, D-NET-349, D-NET-739 -- the S2C 0x0A priority list's distance, its key and its
// tracked-slot despawn. Retail's tile distance is the anchor distance, capped
// and truncated, LESS the row's boundRadius (entity+0), shifted down 16 bits;
// the 1124-tile gate reads it raw, then it floors at 0. In each admitted row's
// turn the list walks the recipient's four tracked slots up to the slot naming
// the row: a slot naming a freed row, the recipient's own entity or a hidden
// slot's entity is cleared and despawned to that recipient alone with a
// one-send S2C 0x12 [u16 handle] (userParam 1), ahead of its 0x0A.
// [orig: Server_BuildEntityPriorityList @0x50E590 - the walk
//  @0x50E736..0x50E867 (the despawn test @0x50E7F3, the clear @0x50E7F9, the
//  0x12 @0x50E835), `sub ecx, [ebx]` @0x50E8DC (pool 1 @0x50EDC9), the gate
//  @0x50E90D..0x50E925, the floor @0x50E92E..0x50E934; Entity_ValidatePtr
//  @0x500910; NetPacket_WriteEntityHandle16 @0x504720; NapiNPServer_SendToConn
//  @0x4C4F52]
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/replication/connection.h>
#include <runtime/replication/connection_fan.h>
#include <runtime/replication/entity_wire_bridge.h>
#include <runtime/world/entity.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/world.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_message_id.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

using namespace opennova;
namespace w = opennova::world;
namespace ns = opennova::replication;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

w::PlayerSpawn at(float x, uint16_t min_slot) {
	w::PlayerSpawn s;
	s.position = {x, 0.0f, 0.0f};
	s.min_entity_slot = min_slot;
	return s;
}

// The recipient's 0x0A run: every datagram its emit queued, in order.
std::vector<ns::Datagram> emit(const w::World &world, ns::Connection &conn,
		const std::vector<GameEntitySnapshot> &ents) {
	(void)ns::emit_connection_s2c(world, conn, ents, 0);
	std::vector<ns::Datagram> out;
	ns::Datagram d;
	auto &ch = *static_cast<ns::LoopbackChannel *>(conn.transport);
	while (ch.client_recv(d)) out.push_back(d);
	return out;
}

bool has_record(const std::vector<ns::Datagram> &run, uint16_t handle) {
	for (const ns::Datagram &d : run) {
		if (d.tag != s2c::PER_FRAME_UPDATE) continue;
		FrameUpdate fu;
		(void)decode_frame_update(d.body.data(), d.body.size(),
				[](uint16_t) { return EntityClass::Player; }, fu);
		for (const FrameUpdateRecord &r : fu.records)
			if (r.handle == handle) return true;
	}
	return false;
}

// The 0x12 handles in a run, in order, and whether each precedes the 0x0A.
std::vector<uint16_t> despawns_before_frame(const std::vector<ns::Datagram> &run) {
	std::vector<uint16_t> out;
	for (const ns::Datagram &d : run) {
		if (d.tag == s2c::PER_FRAME_UPDATE) break;
		if (d.tag == s2c::ENTITY_REMOVE && d.body.size() == 2 && !d.reliable)
			out.push_back(static_cast<uint16_t>(d.body[0] | (d.body[1] << 8)));
	}
	return out;
}

// A row 1125.5 tiles from the anchor fails the gate at age 0; the same row
// with a one-unit boundRadius measures 1124 tiles and passes, so its record
// streams in the first frame.
void test_bound_radius_moves_a_row_inside_the_gate() {
	auto heap = std::make_unique<w::World>();
	w::World &world = *heap;
	world.registry.configure_pool(0, 8);
	const w::EntityHandle me = w::spawn_remote_player(world, at(0.0f, 1));
	const w::EntityHandle far = w::spawn_remote_player(world, at(0.0f, 2));
	const w::Entity &self = *world.registry.get(me);
	const int32_t anchor_x = self.position.x.q16 + self.eye_offset_x;
	std::vector<GameEntitySnapshot> ents = ns::snapshot_world(world);
	for (GameEntitySnapshot &row : ents) {
		if (row.wire_handle != far.packed) continue;
		row.x = anchor_x + 1125 * 65536 + 32768; // 1125.5 tiles along x
		row.y = self.position.y.q16 + self.eye_offset_y;
		row.z = self.position.z.q16 + self.eye_offset_z;
	}

	ns::LoopbackChannel ch_a, ch_b;
	ns::Connection without{&ch_a, ns::TransportMode::Loopback, me, 0};
	CHECK(!has_record(emit(world, without, ents), far.packed));

	for (GameEntitySnapshot &row : ents)
		if (row.wire_handle == far.packed) row.bound_radius_q16 = 65536;
	ns::Connection with{&ch_b, ns::TransportMode::Loopback, me, 0};
	CHECK(has_record(emit(world, with, ents), far.packed));
}

// The tracked-slot despawn: a freed row, the recipient's own entity and a
// hidden slot's entity each draw one one-send 0x12, in slot order, ahead of
// the 0x0A, and the slot reads 0xFFFF; a live unowned row and handles that
// resolve to nothing (pool 5, past the pool's capacity) stay.
void test_tracked_slots_despawn_ahead_of_the_frame() {
	auto heap = std::make_unique<w::World>();
	w::World &world = *heap;
	world.registry.configure_pool(0, 8);
	const w::EntityHandle me = w::spawn_remote_player(world, at(0.0f, 1));
	const w::EntityHandle live = w::spawn_remote_player(world, at(5.0f, 2));
	const w::EntityHandle gone = w::spawn_remote_player(world, at(10.0f, 3));
	const w::EntityHandle hidden = w::spawn_remote_player(world, at(15.0f, 4));
	world.registry.despawn(gone);
	std::vector<GameEntitySnapshot> ents = ns::snapshot_world(world);
	for (GameEntitySnapshot &row : ents)
		if (row.wire_handle == hidden.packed) row.owner_hidden = true; // a spectator's

	ns::LoopbackChannel ch;
	ns::Connection conn{&ch, ns::TransportMode::Loopback, me, 0};
	conn.tracked_handle = {live.packed, gone.packed, hidden.packed, me.packed};
	conn.tracked_score = {7, 7, 7, 7};
	const std::vector<ns::Datagram> run = emit(world, conn, ents);
	CHECK((despawns_before_frame(run) ==
			std::vector<uint16_t>{gone.packed, hidden.packed, me.packed}));
	CHECK(conn.tracked_handle[0] == live.packed);
	CHECK(conn.tracked_handle[1] == 0xFFFF && conn.tracked_handle[2] == 0xFFFF &&
			conn.tracked_handle[3] == 0xFFFF);
	CHECK(has_record(run, live.packed));

	// Nothing left to clear: the next frame sends no 0x12.
	CHECK(despawns_before_frame(emit(world, conn, ents)).empty());

	// Handles that resolve to nothing stay.
	conn.tracked_handle = {0x5001, 0x0008, 0xFFFF, 0xFFFF};
	CHECK(despawns_before_frame(emit(world, conn, ents)).empty());
	CHECK(conn.tracked_handle[0] == 0x5001 && conn.tracked_handle[1] == 0x0008);
}

// D-NET-739: a score the heading singularity drives negative (the cached heading
// byte 0x80 against a heading of 0: abs32(0x80000000) stays negative, -128 after
// the shift, so 2 * headingDelta = -256) is lifted to the row's prior priority,
// 0 for an untracked row (a tracked score is the uplink's u16, zero-extended),
// and keys as a zero-score row: its age leads. A dead recipient's flat score is
// 0 for an unmounted other-team row, so the keys are A = 201 and B = 11.
// [orig: Server_BuildEntityPriorityList - priorPriority = 0 @0x50E6C9 /
//  @0x50E739, the dead-recipient score @0x50EB28..0x50EB3F, 2 * headingDelta
//  @0x50EB80, `if (priorPriority > v48)` @0x50EB8B..0x50EB8F, the key
//  @0x50EB91..0x50EBA6; NetPacket_SerializePlayerState's score store
//  @0x4C1F37..0x4C1F3A; CPairList_ShellSortByValue `jge` @0x526D6B]
void test_negative_score_keys_at_the_prior() {
	auto heap = std::make_unique<w::World>();
	w::World &world = *heap;
	world.registry.configure_pool(0, 8);
	const w::EntityHandle me = w::spawn_remote_player(world, at(0.0f, 1));
	const w::EntityHandle a = w::spawn_remote_player(world, at(5.0f, 2));
	const w::EntityHandle b = w::spawn_remote_player(world, at(10.0f, 3));
	std::vector<GameEntitySnapshot> ents = ns::snapshot_world(world);
	for (GameEntitySnapshot &row : ents) {
		row.team = row.wire_handle == me.packed ? 2 : 1;
		row.euler_z = 0;
		row.tick_speed_q6 = 0;
		row.mounted = false;
		row.state_flags &= ~uint8_t{0x03};
		if (row.wire_handle == me.packed) row.state_flags |= 0x02; // a dead recipient's flat score
	}
	ns::LoopbackChannel ch;
	ns::Connection conn{&ch, ns::TransportMode::Loopback, me, 0};
	conn.s2c_entity_heading[a.slot()] = 0x80;
	conn.s2c_entity_heading[b.slot()] = 0x00;
	conn.s2c_entity_age[a.slot()] = 200; // the sweep makes it 201
	conn.s2c_entity_age[b.slot()] = 10;  // 11
	const std::vector<ns::Datagram> run = emit(world, conn, ents);
	std::vector<uint16_t> order;
	for (const ns::Datagram &d : run) {
		if (d.tag != s2c::PER_FRAME_UPDATE) continue;
		FrameUpdate fu;
		CHECK(decode_frame_update(d.body.data(), d.body.size(),
				[](uint16_t) { return EntityClass::Player; }, fu));
		for (const FrameUpdateRecord &r : fu.records)
			if (r.handle == a.packed || r.handle == b.packed) order.push_back(r.handle);
	}
	CHECK((order == std::vector<uint16_t>{a.packed, b.packed}));
}

// D-NET-349: every row's speed metric is its Position less its mover-entry
// savedLivePose (+0x80), less the same delta of the row it stands on (+0x28),
// the z term halved: sqrt(dx^2 + dy^2 + (dz/2)^2) >> 6, clamped to 255. A
// person (pool 0) walking 0x2000 along x reads 128; the same walk on a deck
// that moved 0x1000 along x reads 64; a 0x10000 rise alone reads
// (0x10000 / 2) >> 6 = 512, clamped to 255; an unstamped row reads 0.
// [orig: Server_BuildEntityPriorityList - pool 0 @0x50E9D9..0x50EA5A (the
//  saved pose @0x50E9B3 / @0x50E9B9 / @0x50E9DC, +0x28 @0x50E9E5..0x50EA13,
//  `sar ecx, 1` @0x50EA15, `sar ecx, 6` @0x50EA4F, 255 @0x50EA52..0x50EA5A);
//  pool 1 @0x50EE7B..0x50EF04]
void test_tick_displacement_reads_the_saved_pose() {
	auto heap = std::make_unique<w::World>();
	w::World &world = *heap;
	world.registry.configure_pool(0, 8);
	world.registry.configure_pool(1, 8);
	const w::EntityHandle walker = w::spawn_remote_player(world, at(0.0f, 1));
	const w::EntityHandle rider = w::spawn_remote_player(world, at(5.0f, 2));
	const w::EntityHandle riser = w::spawn_remote_player(world, at(10.0f, 3));
	const w::EntityHandle still = w::spawn_remote_player(world, at(15.0f, 4));
	w::Entity deck_seed;
	deck_seed.kind = w::EntityKind::Item;
	deck_seed.item_id = 0x050D;
	const w::EntityHandle deck = world.registry.spawn(1, deck_seed);
	const auto moved = [&](w::EntityHandle h, int32_t dx, int32_t dz) {
		w::Entity &e = *world.registry.get(h);
		e.saved_live_pos[0] = e.position.x.q16 - dx;
		e.saved_live_pos[1] = e.position.y.q16;
		e.saved_live_pos[2] = e.position.z.q16 - dz;
		e.saved_live_valid = true;
	};
	moved(walker, 0x2000, 0);
	moved(rider, 0x2000, 0);
	moved(deck, 0x1000, 0);
	world.registry.get(rider)->ground_target = deck;
	moved(riser, 0, 0x10000);
	world.registry.get(still)->saved_live_valid = false;
	const std::vector<GameEntitySnapshot> ents = ns::snapshot_world(world);
	const auto metric = [&](w::EntityHandle h) {
		for (const GameEntitySnapshot &row : ents)
			if (row.wire_handle == h.packed) return int(row.tick_speed_q6);
		return -1;
	};
	CHECK(metric(walker) == 0x2000 >> 6);
	CHECK(metric(rider) == 0x1000 >> 6);
	CHECK(metric(riser) == 255);
	CHECK(metric(still) == 0);
}

} // namespace

int main() {
	test_bound_radius_moves_a_row_inside_the_gate();
	test_tracked_slots_despawn_ahead_of_the_frame();
	test_negative_score_keys_at_the_prior();
	test_tick_displacement_reads_the_saved_pose();
	if (failures == 0) std::printf("priority_list: all passed\n");
	return failures == 0 ? 0 : 1;
}
