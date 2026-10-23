// D-NET-892 (d): a joiner's pool-0 walk calls a decoded person's bound move function (+0x1C4, the
// def's move_function row). org1 and org2 run the chases; the ewep row's update runs over the row's
// words through the World's world::person_ewep_mover (inmatch::run_replica_person_ewep), the row
// leaving after the walk when its stale-ground leg ran (the ground's C2S 0x0F, then the body's);
// every other row runs nothing. The joiner's own row is its World body's. A record's dead -> alive
// edge runs the reset over the snapped pose (the spawn backup and +0x2D4).
// [orig: Entity_UpdateAllEntities @0x4C2100 - the +0x1C4 call @0x4C2472;
//  Entity_UpdateTransformAndTurret @0x440CA0 - the hide @0x440CDD, the stale-ground leg
//  @0x440D03..0x440E37, +0x2D4 @0x440E8E / @0x440EA0, the vehicle ground @0x440EA6..0x440EBE, the
//  root copy @0x441151..0x4411BC, the rider test @0x4411E4, the let-go @0x441366..0x44136D;
//  Entity_ResetToSpawnState @0x4B9610 - the backup @0x4B9632..0x4B9644, +0x2D4 @0x4B96AA]

#include <runtime/inmatch/joiner_role.h>
#include <runtime/replication/client_replica_pipeline.h>
#include <runtime/replication/item_replication_catalog.h>
#include <runtime/world/ai.h>
#include <runtime/world/entity.h>
#include <runtime/world/world.h>

#include <net/npwire/entity_class.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

namespace ns = opennova::replication;
namespace nw = opennova;
namespace w = opennova::world;
using opennova::EntityClass;

int failures = 0;
void expect(bool cond, const char *msg) {
	if (cond) return;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	++failures;
}

constexpr uint16_t kEwepType = 5101, kNullType = 5102, kOrg2Type = 5103;
constexpr uint16_t kNone = 0xFFFF;

std::shared_ptr<const ns::ItemReplicationCatalog> person_catalog() {
	const auto person = [](uint16_t wire_type, const char *move) {
		ns::ItemReplicationDefinition d;
		d.definition_id = ns::ItemReplicationCatalog::kDefinitionIdOffset + wire_type;
		d.item_type = 3;
		d.ai_function = "org2";
		d.move_function = move;
		return d;
	};
	return std::make_shared<const ns::ItemReplicationCatalog>(ns::ItemReplicationCatalog::from_definitions(
			{person(kEwepType, "ewep"), person(kNullType, "null"), person(kOrg2Type, "org2")}));
}

ns::ClientEntityState &staged(ns::ClientReplicaPipeline &view, uint16_t handle, uint16_t type) {
	ns::ClientEntityState &row = view.state().upsert(handle);
	row.cls = EntityClass::Player;
	row.type_id = type;
	row.net_has_compact = true;
	// A staged target 10 units off: the org2 chase snaps past 0x20000.
	row.net_smooth_target[0] = 10 << 16;
	return row;
}

// The walk's dispatch by the bound move row.
void test_the_walk_runs_the_bound_move_row() {
	ns::ClientReplicaPipeline view;
	view.set_remote_motion_mode(true);
	view.set_item_catalog(person_catalog());
	std::vector<uint16_t> ewep_calls;
	bool destroy = false;
	view.set_person_ewep_mover([&](ns::ClientEntityState &row) {
		ewep_calls.push_back(row.handle);
		return destroy;
	});
	staged(view, 0x0021, kEwepType).resolved_ground = 0x1004;
	staged(view, 0x0022, kNullType);
	staged(view, 0x0023, kOrg2Type);
	staged(view, 0x0024, kEwepType); // the joiner's own row
	view.tick_remote_motion(/*self_handle=*/0x0024);
	expect(ewep_calls.size() == 1 && ewep_calls[0] == 0x0021,
			"the ewep row's update runs, once, and never for the own row");
	expect(view.state().find(0x0021)->x == 0, "the ewep row takes no chase");
	expect(view.state().find(0x0022)->x == 0, "the null row runs nothing");
	expect(view.state().find(0x0023)->x == (10 << 16), "an org2 row keeps its chase");
	// The update's stale-ground leg: the ground's C2S 0x0F, then the body's, and the row leaves.
	destroy = true;
	view.tick_remote_motion(0x0024);
	expect(view.state().find(0x0021) == nullptr, "the destroyed ewep row leaves after the walk");
	const std::vector<uint16_t> repairs = view.drain_carrier_repair_requests();
	expect(repairs.size() == 2 && repairs[0] == 0x1004 && repairs[1] == 0x0021,
			"the ground's request, then the body's");
	// Without a catalog entry the row keeps its class's chase.
	ns::ClientReplicaPipeline bare;
	bare.set_remote_motion_mode(true);
	bare.set_person_ewep_mover([&](ns::ClientEntityState &) { return false; });
	staged(bare, 0x0025, kEwepType);
	bare.tick_remote_motion(kNone);
	expect(bare.state().find(0x0025)->x == (10 << 16), "an unresolved type keeps its chase");
}

struct Rig {
	std::unique_ptr<w::World> wp = std::make_unique<w::World>();
	w::World &world = *wp;
	ns::ClientState state;
	w::EntityHandle ground;
	Rig() {
		world.registry.configure_pool(0, 8);
		world.registry.configure_pool(1, 8);
		w::Entity hull;
		hull.kind = w::EntityKind::Item;
		hull.has_item_def = true;
		hull.item_type = 1; // a vehicle
		hull.health = 345;
		hull.position.set_q16({10 << 16, 20 << 16, 3 << 16});
		hull.heading = 0x12340000;
		hull.pitch_bam = 0x01000000;
		hull.roll_bam = 0x02000000;
		ground = world.registry.spawn(1, hull);
	}
	ns::ClientEntityState &row(uint16_t handle) {
		ns::ClientEntityState &r = state.upsert(handle);
		r.cls = EntityClass::Player;
		r.type_id = kEwepType;
		r.resolved_ground = ground.packed;
		return r;
	}
};

// The update over the row's words: with a zero +0x2D4 (no reset ran) only the legs ahead of its
// gate; with one, a vehicle ground's block (Flags bits 0..2 clear, its Health) and the unanchored
// root copy of the ground's pose.
void test_the_update_runs_over_the_rows_words() {
	{
		Rig r;
		ns::ClientEntityState &row = r.row(0x0031);
		row.state_flags = 0x07;
		expect(!opennova::inmatch::run_replica_person_ewep(r.world, r.state, row, kNone),
				"no stale ground, no destroy");
		expect(row.x == 0 && row.state_flags == 0x07, "a zero +0x2D4 ends the update at its gate");
	}
	{
		Rig r;
		ns::ClientEntityState &row = r.row(0x0031);
		row.state_flags = 0x07;
		row.reset_yaw = 0x40000000;
		opennova::inmatch::run_replica_person_ewep(r.world, r.state, row, kNone);
		expect((row.state_flags & 0x07) == 0, "a vehicle ground clears Flags bits 0..2");
		expect(row.health_known && row.health_word == 345, "and copies its Health");
		expect(row.x == (10 << 16) && row.y == (20 << 16) && row.z == (3 << 16) &&
				row.heading_bam == 0x12340000 && row.pitch_bam == 0x01000000 &&
				row.roll_bam == 0x02000000,
				"the unanchored body takes the ground's pose");
	}
	{
		// A dead PlayerControl ground hides the body and ends the update.
		Rig r;
		w::Entity *hull = r.world.registry.get(r.ground);
		hull->item_attrib |= w::kItemAttribPlayerControl;
		hull->flags |= w::kEntityFlagDead;
		ns::ClientEntityState &row = r.row(0x0031);
		row.reset_yaw = 0x40000000;
		opennova::inmatch::run_replica_person_ewep(r.world, r.state, row, kNone);
		expect((row.state_flags & 0x01) != 0 && row.x == 0, "the hide, and nothing after it");
	}
}

// The stale-ground leg on a session peer off the authority: a ground the shared destroy removed
// (its mark) on a tick whose low seven bits are all set.
void test_the_stale_ground_leg() {
	for (const uint32_t tick : {0x7Fu, 0x7Eu}) {
		Rig r;
		r.world.rules.mp_session = true;
		r.world.rules.mp_session_peer = true;
		r.world.rules.logic_authority = false;
		r.world.logic_tick = tick;
		const uint16_t gone = 0x1006;
		r.world.destroyed_mark = [gone](uint16_t handle) { return handle == gone; };
		ns::ClientEntityState &row = r.row(0x0031);
		row.resolved_ground = gone;
		const bool destroyed = opennova::inmatch::run_replica_person_ewep(r.world, r.state, row, kNone);
		expect(destroyed == (tick == 0x7Fu), "the leg runs on the 0x7F tick alone");
		expect(r.world.out.entity_info_requests.empty(),
				"its requests leave by wire handle through the replica's queue");
	}
}

// The +0x170 leg over a decoded rider: kept while its row's seat link names the body (+0x2AC
// takes 1), let go otherwise.
void test_the_rider_leg() {
	for (const bool rides : {true, false}) {
		Rig r;
		ns::ClientEntityState &rider = r.state.upsert(0x0040);
		rider.cls = EntityClass::Player;
		rider.carrier_handle = rides ? 0x0031 : 0x0032;
		rider.mount_bone = 3;
		ns::ClientEntityState &row = r.row(0x0031);
		row.reset_yaw = 0x40000000;
		row.ride_occupant = 0x0040;
		row.spawn_phase = 40;
		opennova::inmatch::run_replica_person_ewep(r.world, r.state, row, kNone);
		ns::ClientEntityState *body = r.state.find(0x0031);
		expect(body->ride_occupant == (rides ? 0x0040 : kNone), "a rider off the body is let go");
		expect(body->spawn_phase == (rides ? 1 : 40), "+0x2AC takes 1 while the link holds");
	}
}

// A player compact at world x (16.16) with its wire Flags byte, unmounted.
nw::FrameUpdate player_frame(uint16_t handle, int32_t x, uint8_t state_flags) {
	nw::FrameUpdate fu;
	fu.flags2 = 0;
	fu.carried_handle = 0xFFFF;
	fu.health = 100;
	nw::FrameUpdateRecord r;
	r.handle = handle;
	r.type_id = kEwepType;
	r.cls = EntityClass::Player;
	r.player.carrier_handle = 0xFFFF;
	r.player.pos_x_compressed = nw::network_compress_fixedpoint(x);
	r.player.pos_y_compressed = nw::network_compress_fixedpoint(0);
	r.player.pos_z_compressed = nw::network_compress_fixedpoint(0);
	r.player.yaw_byte = 0x40;
	r.player.state_flags = state_flags;
	r.player.anim_def_index = 0xFF;
	r.player.health_class_byte = 0x28;
	fu.records.push_back(r);
	return fu;
}

// A record's dead -> alive edge runs the reset over the snapped pose: the spawn backup takes
// Position and +0x2D4 the Yaw; a live record before it leaves both at the spawn's clear.
void test_the_respawn_edge_runs_the_reset() {
	ns::ClientReplicaPipeline view([](uint16_t) { return EntityClass::Player; });
	view.set_remote_motion_mode(true);
	const uint16_t handle = 0x0033;
	view.state().upsert(handle).type_id = kEwepType;
	view.apply(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(player_frame(handle, 4 << 16, 0)));
	const ns::ClientEntityState *row = view.state().find(handle);
	expect(row->reset_yaw == 0 && row->spawn_backup[0] == 0, "a live record runs no reset");
	view.apply(nw::s2c::PER_FRAME_UPDATE,
			nw::encode_frame_update(player_frame(handle, 4 << 16, 0x02)));
	view.apply(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(player_frame(handle, 9 << 16, 0)));
	row = view.state().find(handle);
	expect(row->respawn_revision == 1, "the dead -> alive edge");
	expect(row->spawn_backup[0] == row->x && row->x != 0 && row->spawn_backup[1] == row->y &&
			row->spawn_backup[2] == row->z, "the backup takes the snapped Position");
	expect(row->reset_yaw == 0x40000000 && row->reset_yaw == row->heading_bam,
			"+0x2D4 takes the Yaw");
}

} // namespace

int main() {
	test_the_respawn_edge_runs_the_reset();
	test_the_walk_runs_the_bound_move_row();
	test_the_update_runs_over_the_rows_words();
	test_the_stale_ground_leg();
	test_the_rider_leg();
	if (failures == 0) std::puts("npruntime_replica_person_ewep: OK");
	return failures == 0 ? 0 : 1;
}
