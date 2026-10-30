// D-NET-449: a player slot whose body a script's removeSSN destroyed keeps its entity
// pointer. Retail's 0x0A gate has no lifetime test (the active byte, the pointer, state 6,
// the net player [orig: Server_SendEntityStateToPlayer @0x517BAB..0x517BE2]), so the
// stale slot's 0x0A keeps going out: anchored at the zeroed entry while the entry is free
// [orig: Entity_Destroy's memset @0x43EA70], then at whatever body the pool reused the
// entry for (a zeroed ItemTypeIndex reads free, Pool_AllocEntry @0x44225C; the add passes
// includeInUse 1 @0x43C3DF). Every slot's writer stores into that body in the slot walk's
// order: the hide bit cleared (@0x4FF6D0..0x4FF6D7), a spectator slot's restamp (hide, -1,
// Health 1, @0x4FF6FA..0x4FF728) and the re-set for its own deploy-screen flag
// (+0x15F38 & 0x10, @0x4FF7AA..0x4FF7B8), each slot's records reading the body as the
// slots before it left it [orig: Server_TickUpdate's per-slot loop @0x51E3A0..0x51E47C].
// Ruled (a), the maintainer 2026-10-20 (Q3-13b). The world and slots are the test's own.

#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_entity_state.h>
#include <runtime/inmatch/server_tick.h> // connections_in_slot_order (the frame update's walk)
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/replication/connection_fan.h>
#include <runtime/replication/entity_wire_bridge.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/world.h>

#include <net/protocol/protocol.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "conn_fixture.h"

namespace {

using namespace opennova;
namespace inmatch = opennova::inmatch;
namespace ns = opennova::replication;
namespace w = opennova::world;
using conn_fixture::make_seeded_conn;

int failures = 0;
#define CHECK(c)                                                                               \
	do {                                                                                       \
		if (!(c)) {                                                                            \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                           \
			++failures;                                                                        \
		}                                                                                      \
	} while (0)

struct Rig {
	std::unique_ptr<w::World> wp = std::make_unique<w::World>();
	w::World &world = *wp;
	std::unique_ptr<inmatch::NapiNPServerCtx> ctx = std::make_unique<inmatch::NapiNPServerCtx>();
	std::vector<std::unique_ptr<ns::LoopbackChannel>> wires;

	Rig() {
		world.registry.configure_pool(0, 8);
		world.rules.mp_session = true;
		ctx->world = &world;
		ctx->is_authority = 1;
		ctx->is_in_session = 1;
		ctx->np_protocol.identity = title_protocol("jo");
	}
	w::EntityHandle spawn(float x) {
		w::PlayerSpawn s;
		s.position = {x, 10.0f, 2.0f};
		s.team = 1;
		s.net_id = 0;
		return w::spawn_remote_player(world, s);
	}
	inmatch::NapiNPConnection &add(uint32_t id, uint8_t slot, w::EntityHandle body) {
		wires.push_back(std::make_unique<ns::LoopbackChannel>());
		ctx->np_protocol.connection_list.push_back(make_seeded_conn(
				id, 1, wires.back().get(), ns::TransportMode::Client, body, true));
		inmatch::NapiNPConnection &c = ctx->np_protocol.connection_list.back();
		c.reply.player_slot = slot;
		return c;
	}
	inmatch::NapiNPConnection &conn(size_t i) { return ctx->np_protocol.connection_list[i]; }
	const GameEntitySnapshot *row(const std::vector<GameEntitySnapshot> &ents,
			w::EntityHandle h) const {
		for (const GameEntitySnapshot &r : ents)
			if (r.wire_handle == h.packed) return &r;
		return nullptr;
	}
};

// Before the entry is reused the stale slot's 0x0A still goes out, anchored at the zeroed
// entry (the origin), where ours refused the stale binding (D-NET-121).
void test_a_freed_entry_anchors_at_the_origin() {
	Rig r;
	const w::EntityHandle body = r.spawn(100.0f);
	r.add(3, 1, body);
	r.world.registry.despawn(body);
	CHECK(r.world.registry.get(body) == nullptr);
	CHECK(ns::owned_entity_for_emit(r.world, r.conn(0).link) != nullptr);
	const w::Entity *read = ns::owned_entity_for_emit(r.world, r.conn(0).link);
	CHECK(read != nullptr && read->position.x.q16 == 0 && read->position.y.q16 == 0 &&
			read->position.z.q16 == 0 && read->health == 0);
	std::vector<GameEntitySnapshot> ents = inmatch::snapshot_entity_states(*r.ctx, r.world);
	std::vector<uint8_t> frame;
	CHECK(inmatch::write_entity_state(*r.ctx, r.world, r.conn(0), ents, frame));
	CHECK(!frame.empty());
}

// Once a later spawn reuses the entry, the stale slot's 0x0A reads the new body (the
// packed handle, no lifetime test), and its writer stores into it.
void test_a_reused_entry_aliases_the_new_body() {
	Rig r;
	const w::EntityHandle old_body = r.spawn(100.0f);
	r.add(3, 1, old_body);
	r.world.registry.despawn(old_body);
	const w::EntityHandle new_body = r.spawn(250.0f);
	CHECK(new_body == old_body);
	r.add(4, 2, new_body);
	const w::Entity *read = ns::owned_entity_for_emit(r.world, r.conn(0).link);
	CHECK(read == r.world.registry.get(new_body));
	r.world.registry.get(new_body)->flags |= w::kEntityFlagCarried; // hidden
	std::vector<GameEntitySnapshot> ents = inmatch::snapshot_entity_states(*r.ctx, r.world);
	std::vector<uint8_t> frame;
	CHECK(inmatch::write_entity_state(*r.ctx, r.world, r.conn(0), ents, frame));
	// The stale non-spectator slot's writer cleared the reused body's hide bit.
	CHECK((r.world.registry.get(new_body)->flags & w::kEntityFlagCarried) == 0u);
}

// Two stale slots of different kinds over one reused body: a spectator slot (the restamp:
// hide, -1, Health 1) and a deployed non-spectator (the hide clear), with the body's own
// slot on its deploy screen (the re-set). What a slot's records read of the body is the
// body as the slots before it in the walk left it: walked spectator, deployed, own, the
// deployed slot reads it hidden at Health 1 and the own slot unhidden; walked deployed,
// spectator, own, the spectator slot reads it unhidden at full health and the own slot
// hidden at Health 1. The body ends hidden either way (its own slot's re-set).
void test_the_walk_order_decides_what_each_slot_reads() {
	for (const bool spectator_first : {true, false}) {
		Rig r;
		const w::EntityHandle first = r.spawn(100.0f);
		r.add(3, spectator_first ? 1 : 2, first); // the spectator's stale slot
		r.world.registry.despawn(first);
		const w::EntityHandle second = r.spawn(150.0f);
		r.add(4, spectator_first ? 2 : 1, second); // the deployed stale slot
		r.world.registry.despawn(second);
		const w::EntityHandle body = r.spawn(250.0f);
		CHECK(first == second && second == body);
		r.add(5, 3, body);
		r.conn(0).link.spectator = true;
		r.conn(2).link.respawn_pending = true; // the new body's slot, on its deploy screen
		w::Entity *live = r.world.registry.get(body);
		const int32_t full = live->health;
		std::vector<GameEntitySnapshot> ents =
				inmatch::snapshot_entity_states(*r.ctx, r.world);
		// The frame update's own walk: the slot table's order, not the list's (the join
		// order) [orig: Server_TickUpdate's per-slot loop @0x51E3A0..0x51E47C].
		const std::vector<inmatch::NapiNPConnection *> walk =
				inmatch::connections_in_slot_order(*r.ctx);
		CHECK(walk.size() == 3);
		if (walk.size() != 3) continue;
		CHECK(walk[0] == (spectator_first ? &r.conn(0) : &r.conn(1)));
		CHECK(walk[2] == &r.conn(2));
		std::vector<uint8_t> after[3];
		for (int i = 0; i < 3; ++i) {
			std::vector<uint8_t> frame;
			CHECK(inmatch::write_entity_state(*r.ctx, r.world, *walk[static_cast<size_t>(i)], ents,
					frame));
			const GameEntitySnapshot *row = r.row(ents, body);
			CHECK(row != nullptr);
			if (row != nullptr) after[i] = {row->state_flags, static_cast<uint8_t>(row->health)};
		}
		if (spectator_first) {
			// The spectator's restamp, then the deployed slot's clear, then the own re-set.
			CHECK((after[0][0] & 1u) != 0 && after[0][1] == 1);
			CHECK((after[1][0] & 1u) == 0 && after[1][1] == 1);
		} else {
			// The deployed slot's clear, then the spectator's restamp.
			CHECK((after[0][0] & 1u) == 0 && after[0][1] == static_cast<uint8_t>(full));
			CHECK((after[1][0] & 1u) != 0 && after[1][1] == 1);
		}
		CHECK((after[2][0] & 1u) != 0);
		CHECK(live->health == 1 && live->damage_state == -1);
	}
}

} // namespace

int main() {
	test_a_freed_entry_anchors_at_the_origin();
	test_a_reused_entry_aliases_the_new_body();
	test_the_walk_order_decides_what_each_slot_reads();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("PASS npruntime_stale_slot_alias\n");
	return 0;
}
