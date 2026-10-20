// D-NET-439, D-NET-461, D-NET-462 -- the S2C 0x0A writer's stores on the slot's
// body, slot by slot. Retail's per-slot loop builds each slot's priority list,
// then runs NetPacket_WritePlayerState, whose stores clear the body's hide bit
// and re-set it for a spectator or a held slot, restamp a spectator, and while
// the pre-round timer runs clear the dead bit, then writes the slot's records
// and sends. So the cleared dead bit lives only in that 0x0A: the same frame's
// entity update latches it again on an unhidden body at Health <= 0 and on the
// authority re-runs the death, whose 0x13 leads the next frame; a slot's
// records read the bodies of the slots before it after their stores and those
// after it before theirs; and a held body the 0x0E deploys stays hidden, out
// of the tick's humans count, until its slot's next 0x0A.
// [orig: Server_TickUpdate's per-slot loop @0x51E3A0..0x51E47C (the holdoff
//  gate @0x51E3D6, the call @0x51E3E4), the humans count @0x51D89A;
//  Server_SendEntityStateToPlayer @0x517BA0 - the list @0x517C1E, the writer
//  @0x517C76, the records @0x517C81; NetPacket_WritePlayerState @0x4FF6B0 - the
//  hide clear @0x4FF6D7, the pre-round clear @0x4FF72F..0x4FF738, the held
//  re-set @0x4FF7B8, the +360 / +368 tests @0x4FF83C / @0x4FF86D;
//  Server_DeployPlayer's one Flags store @0x51787A; Server_BuildEntitySlotLists
//  hidden skip @0x4F9820; Entity_UpdateInfantryPlayerBody @0x4B4BF1..0x4B4CEA]
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_message_dispatch.h>
#include <runtime/inmatch/server_session.h>
#include <runtime/inmatch/server_tick.h>
#include <runtime/inmatch/udp_session_transport.h>
#include <runtime/world/infantry.h> // compute_death_anim_state
#include <runtime/world/entity.h>
#include <runtime/world/match.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/world.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/protocol_message.h>

#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

#include "conn_fixture.h"

using namespace opennova;
namespace w = opennova::world;
namespace ns = opennova::replication;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

// A dedicated team host with three remote players in pool-0 slots 1, 2, 3
// (teams 1, 2, 2), every frame a send boundary, no spawn zones.
struct Fixture {
	inmatch::NapiNPServerCtx ctx;
	std::unique_ptr<w::World> heap = std::make_unique<w::World>();
	w::World &world = *heap;
	std::vector<ns::UdpSessionTransport> transports;
	std::vector<w::EntityHandle> players;

	Fixture() {
		inmatch::set_connection_mode(ctx, inmatch::ConnectionMode::HostOnly);
		ctx.is_in_session = 1;
		ctx.config.game_type = 0x10000u; // TDM
		ctx.config.respawn_timeout = 1;
		ctx.config.death_messages = 1;
		world.rules.mp_session = true;
		world.rules.logic_authority = true;
		world.registry.configure_pool(0, 16);
		w::MatchRules rules;
		rules.game_type = 0x10000u;
		world.match.configure(rules);
		ctx.world = &world;
		transports.reserve(3);
		for (size_t i = 0; i < 3; ++i) {
			transports.emplace_back(ns::UdpSessionTransport::Role::Host);
			w::PlayerSpawn spawn;
			spawn.position = {10.0f * float(i), 0.0f, 0.0f};
			spawn.team = static_cast<uint8_t>(i == 0 ? 1 : 2);
			spawn.min_entity_slot = static_cast<uint16_t>(i + 1);
			players.push_back(w::spawn_remote_player(world, spawn));
			world.match.upsert_player({players.back(), static_cast<uint8_t>(i + 1), "P"});
		}
		for (size_t i = 0; i < 3; ++i) {
			inmatch::NapiNPConnection c = conn_fixture::make_seeded_conn(
					static_cast<uint32_t>(inmatch::kFirstJoinerDcb + i), 1, &transports[i],
					ns::TransportMode::Client, players[i], true);
			c.reply.player_slot = static_cast<uint8_t>(i + 1);
			c.admission_stage = inmatch::GameAdmissionStage::Complete;
			c.s2c_send_boundary_open = true;
			ctx.np_protocol.connection_list.push_back(std::move(c));
		}
		inmatch::Server_TickUpdate(ctx); // the first frame's one-second service
		for (size_t i = 0; i < 3; ++i) (void)drain(i);
	}
	inmatch::NapiNPConnection &conn(size_t i) { return ctx.np_protocol.connection_list[i]; }
	w::Entity &entity(size_t i) { return *world.registry.get(players[i]); }
	bool dead_bit(size_t i) {
		return ((entity(i).flags | entity(i).engine_flags) & w::kEntityFlagDead) != 0u;
	}
	bool hidden(size_t i) { return (entity(i).flags & w::kEntityFlagCarried) != 0u; }
	// Player k's lethal hit on body i (Health 0, the shooter and a staged clip).
	void hit(size_t i, size_t k) {
		entity(i).health = 0;
		entity(i).last_attacker = players[k];
		entity(i).death_anim_state = w::compute_death_anim_state(0, 0, w::death_cause::kGeneric);
	}
	std::vector<ns::Datagram> drain(size_t i) {
		std::vector<ns::Datagram> out;
		ns::Datagram d;
		while (transports[i].pop_outbound(d)) out.push_back(d);
		return out;
	}
	// The 0x13s naming body i in a drained run.
	int deaths_of(const std::vector<ns::Datagram> &run, size_t i) {
		int n = 0;
		for (const ns::Datagram &d : run)
			if (d.tag == s2c::ENTITY_DEATH && d.body.size() >= 2 &&
					static_cast<uint16_t>(d.body[0] | (d.body[1] << 8)) == players[i].packed)
				++n;
		return n;
	}
	// The last 0x0A in a drained run, decoded.
	static FrameUpdate frame_of(const std::vector<ns::Datagram> &run) {
		FrameUpdate frame;
		for (const ns::Datagram &d : run) {
			if (d.tag != s2c::PER_FRAME_UPDATE) continue;
			frame = FrameUpdate{};
			(void)decode_frame_update(d.body.data(), d.body.size(),
					[](uint16_t) { return EntityClass::Player; }, frame);
		}
		return frame;
	}
	// Body i's record in a frame, or null.
	const FrameUpdateRecord *record_of(const FrameUpdate &frame, size_t i) const {
		for (const FrameUpdateRecord &r : frame.records)
			if (r.handle == players[i].packed) return &r;
		return nullptr;
	}
	// Kill body i by body k's hit and let the death's sends go out.
	void kill(size_t i, size_t k) {
		hit(i, k);
		inmatch::Server_TickUpdate(ctx); // the edge latches and runs the transaction
		inmatch::Server_TickUpdate(ctx); // its sends lead this frame
		for (size_t s = 0; s < 3; ++s) (void)drain(s);
	}
};

// A dead, unhidden, unheld body through a pre-round frame: its slot's 0x0A
// carries the dead bit clear (the header's +360 test reads it after the clear,
// so the delay byte is 0, and the body's own record shows bit 1 clear); the
// same frame's entity update latches the bit again and re-runs the death,
// whose 0x13 leads the next frame. Outside the pre-round nothing clears it.
void test_preround_0x0a_clears_the_dead_bit_and_the_edge_rekills() {
	Fixture f;
	f.kill(0, 1);
	CHECK(f.dead_bit(0));
	CHECK(f.conn(0).link.respawn_delay_seconds == 3u);

	f.world.preround_delay_seconds = 5;
	f.conn(0).link.s2c_phase = 0xFF; // the writer's ++ lands on the phase-0 block
	inmatch::Server_TickUpdate(f.ctx);
	const FrameUpdate frame = Fixture::frame_of(f.drain(0));
	CHECK(frame.weapon.present);
	CHECK(frame.weapon.preround_timer == 5u);
	CHECK(frame.weapon.slot_state360 == 0u);
	const FrameUpdateRecord *own = f.record_of(frame, 0);
	CHECK(own != nullptr && (own->player.state_flags & 0x02u) == 0u);
	CHECK(f.dead_bit(0)); // the edge latched it again in the frame's update
	(void)f.drain(1);

	inmatch::Server_TickUpdate(f.ctx);
	const std::vector<ns::Datagram> next = f.drain(1);
	CHECK(f.deaths_of(next, 0) == 1); // the re-kill's 0x13
	CHECK(f.dead_bit(0));

	// Out of the pre-round the writer leaves the bit: no clear, no re-kill.
	f.world.preround_delay_seconds = 0;
	inmatch::Server_TickUpdate(f.ctx); // the last pre-round frame's sends go out
	for (size_t s = 0; s < 3; ++s) (void)f.drain(s);
	f.conn(0).link.s2c_phase = 0xFF;
	inmatch::Server_TickUpdate(f.ctx);
	const FrameUpdate after = Fixture::frame_of(f.drain(0));
	CHECK(after.weapon.present && after.weapon.slot_state360 == 3u);
	const FrameUpdateRecord *after_own = f.record_of(after, 0);
	CHECK(after_own != nullptr && (after_own->player.state_flags & 0x02u) != 0u);
	inmatch::Server_TickUpdate(f.ctx);
	CHECK(f.deaths_of(f.drain(1), 0) == 0);
}

// Slot by slot: with bodies 0 and 1 dead in the pre-round, slot 0's records
// read body 1 before its own writer clears it, and slot 1's read body 0 after
// slot 0's writer did.
void test_each_slot_reads_the_slots_before_it_after_their_stores() {
	Fixture f;
	f.kill(0, 2);
	f.kill(1, 2);
	CHECK(f.dead_bit(0) && f.dead_bit(1));
	f.world.preround_delay_seconds = 5;
	inmatch::Server_TickUpdate(f.ctx);
	const FrameUpdate first = Fixture::frame_of(f.drain(0));
	const FrameUpdate second = Fixture::frame_of(f.drain(1));
	const FrameUpdateRecord *first_of_1 = f.record_of(first, 1);
	const FrameUpdateRecord *second_of_0 = f.record_of(second, 0);
	CHECK(first_of_1 != nullptr && (first_of_1->player.state_flags & 0x02u) != 0u);
	CHECK(second_of_0 != nullptr && (second_of_0->player.state_flags & 0x02u) == 0u);
	const FrameUpdateRecord *second_own = f.record_of(second, 1);
	CHECK(second_own != nullptr && (second_own->player.state_flags & 0x02u) == 0u);
}

// The writes follow the slot table, not the join order: with slot 2's
// connection joined first, slot 1's records still read body 1 (slot 2's) before
// its writer clears it.
void test_slots_write_in_slot_order_not_join_order() {
	Fixture f;
	f.kill(0, 2);
	f.kill(1, 2);
	std::swap(f.ctx.np_protocol.connection_list[0], f.ctx.np_protocol.connection_list[1]);
	f.world.preround_delay_seconds = 5;
	inmatch::Server_TickUpdate(f.ctx);
	const FrameUpdate slot1 = Fixture::frame_of(f.drain(0));
	const FrameUpdate slot2 = Fixture::frame_of(f.drain(1));
	const FrameUpdateRecord *slot1_of_body1 = f.record_of(slot1, 1);
	const FrameUpdateRecord *slot2_of_body0 = f.record_of(slot2, 0);
	CHECK(slot1_of_body1 != nullptr && (slot1_of_body1->player.state_flags & 0x02u) != 0u);
	CHECK(slot2_of_body0 != nullptr && (slot2_of_body0->player.state_flags & 0x02u) == 0u);
}

// A held body the 0x0E deploys in the receive pump stays hidden, out of the
// next tick's humans count, until that tick's 0x0A clears the bit; a slot
// whose send boundary is shut keeps it until its next 0x0A. While the slot
// is held, every 0x0A re-sets the bit.
void test_a_deployed_held_body_stays_hidden_until_its_0x0a() {
	Fixture f;
	f.conn(2).link.respawn_pending = true;
	f.world.match.set_player_respawn_pending(f.players[2], true);
	inmatch::Server_TickUpdate(f.ctx);
	CHECK(f.hidden(2)); // the held slot's 0x0A set it, after the tick's count
	CHECK(f.world.cached.humans == 3);
	inmatch::Server_TickUpdate(f.ctx);
	CHECK(f.hidden(2));
	CHECK(f.world.cached.humans == 2);

	inmatch::ServerDispatchInputs inputs;
	inputs.server_ctx = &f.ctx;
	(void)inmatch::dispatch_session_replies(f.ctx.config, f.conn(2),
			{make_protocol_message(c2s::RESPAWN_REQUEST, {0xFF, 0xFF})}, 100,
			f.ctx.np_protocol.connection_list, &f.world, inputs);
	CHECK(!f.conn(2).link.respawn_pending);
	CHECK(f.hidden(2)); // the deploy stores nothing to the bit

	f.conn(2).s2c_send_boundary_open = false;
	inmatch::Server_TickUpdate(f.ctx);
	CHECK(f.hidden(2));
	CHECK(f.world.cached.humans == 2);

	f.conn(2).s2c_send_boundary_open = true;
	inmatch::Server_TickUpdate(f.ctx);
	CHECK(f.world.cached.humans == 2); // counted ahead of the fan
	CHECK(!f.hidden(2));
	inmatch::Server_TickUpdate(f.ctx);
	CHECK(f.world.cached.humans == 3);
}

// A player body a script hid (WacCmd_HideSsn's port sets every view of the
// Flags bit 0) is unhidden by its slot's next 0x0A, every view, as retail's
// one dword is: the tick after counts it among the humans again.
// [orig: NetPacket_WritePlayerState `and [edi+24h], ecx` @0x4FF6D7;
//  WacCmd_HideSsn @0x4F7750; Server_BuildEntitySlotLists's hidden skip @0x4F9820]
void test_a_script_hidden_body_unhides_at_its_0x0a() {
	Fixture f;
	f.entity(1).flags |= w::kEntityFlagCarried;
	f.entity(1).engine_flags |= w::kEntityFlagCarried;
	f.entity(1).hidden = true;
	inmatch::Server_TickUpdate(f.ctx);
	CHECK(f.world.cached.humans == 2); // counted ahead of the fan
	CHECK((f.entity(1).flags & w::kEntityFlagCarried) == 0u);
	CHECK((f.entity(1).engine_flags & w::kEntityFlagCarried) == 0u);
	CHECK(!f.entity(1).hidden);
	inmatch::Server_TickUpdate(f.ctx);
	CHECK(f.world.cached.humans == 3);
}

} // namespace

int main() {
	test_preround_0x0a_clears_the_dead_bit_and_the_edge_rekills();
	test_each_slot_reads_the_slots_before_it_after_their_stores();
	test_slots_write_in_slot_order_not_join_order();
	test_a_deployed_held_body_stays_hidden_until_its_0x0a();
	test_a_script_hidden_body_unhides_at_its_0x0a();
	if (failures == 0) std::printf("player_state_writer: all passed\n");
	return failures == 0 ? 0 : 1;
}
