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

#include <base/io/rotating_prng.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <utility>
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

// The joiner's production walk (the own-body split and the pool-1 interleave) parks every pool-0
// row and visits it in its parked half. A person bound to an item row runs that row's update
// alone there: none of the org2 body's legs (the lean decay, the arms dip with the torso roll,
// the seat follow, the selection, the lean ramp), and the frame's recompose gives it no seat
// pose either; an org2 rider keeps every one.
// [orig: Entity_UpdateAllEntities @0x4C2100 - the +0x1C4 call @0x4C2472;
//  Entity_UpdateInfantryPlayerBody @0x4B40E0 - the decay @0x4B5C97, the dip
//  @0x4B5CAB..0x4B5CE7, the site selector @0x4B637C..0x4B638F, the ramp @0x4B7DBF]
void test_the_split_walk_runs_no_body_legs_over_an_item_row() {
	auto view = std::make_unique<ns::ClientReplicaPipeline>();
	view->set_remote_motion_mode(true);
	view->set_self_body_split(true);
	view->set_pool1_interleave(true);
	view->set_item_catalog(person_catalog());
	std::vector<uint16_t> ewep_calls;
	view->set_person_ewep_mover([&](ns::ClientEntityState &row) {
		ewep_calls.push_back(row.handle);
		return false;
	});
	ns::ClientEntityState &carrier = view->state().upsert(0x1001);
	carrier.cls = EntityClass::Vehicle;
	carrier.x = 100 << 16;
	const auto rider = [&](uint16_t handle, uint16_t type) {
		ns::ClientEntityState &row = staged(*view, handle, type);
		row.x = 5 << 16;
		row.carrier_handle = 0x1001;
		row.net_seat_valid = true;
		row.net_seat_local[0] = 1 << 16;
		row.lean_angle = 0x01000000;
		row.move_input = static_cast<uint8_t>(w::Entity::kMoveOrderLeanLeft);
		row.pitch_kick_accum = 0x00100000;
		row.arms_dip_ticks = 4;
		row.roll_bam = 0x10000000;
	};
	rider(0x0021, kEwepType);
	rider(0x0023, kOrg2Type);
	staged(*view, 0x0024, kOrg2Type); // the joiner's own row
	view->tick_remote_motion(0x0024);
	view->tick_remote_motion_after_self();
	view->refresh_carried_entities();
	const ns::ClientEntityState *ewep = view->state().find(0x0021);
	const ns::ClientEntityState *org2 = view->state().find(0x0023);
	expect(ewep_calls.size() == 1 && ewep_calls[0] == 0x0021, "the parked item row runs its update");
	expect(ewep->x == (5 << 16), "the item row takes no seat pose, in its visit or the recompose");
	expect(ewep->lean_angle == 0x01000000, "no lean decay and no lean ramp");
	expect(ewep->pitch_kick_accum == 0x00100000 && ewep->arms_dip_ticks == 4 &&
			ewep->torso_roll == 0, "no arms dip and no torso roll");
	expect(org2->x != (5 << 16), "the org2 rider takes its seat");
	expect(org2->lean_angle != 0x01000000, "and its lean legs");
	expect(org2->pitch_kick_accum != 0x00100000 && org2->arms_dip_ticks != 4 &&
			org2->torso_roll != 0, "and its dip and roll");
}

// The walk with no own-body split (the HostClient view's, the authority never interpolating)
// runs the lean decay, the arms dip and the lean ramp from its own legs over every row: an
// item-row person takes none of them there either, an org2 row every one.
// [orig: Entity_UpdateAllEntities @0x4C2100 - the +0x1C4 call @0x4C2472;
//  Entity_UpdateInfantryPlayerBody @0x4B40E0 - the decay @0x4B5C97, the dip
//  @0x4B5CAB..0x4B5CE7, the ramp @0x4B7DBF]
void test_the_unsplit_walk_runs_no_body_legs_over_an_item_row() {
	auto view = std::make_unique<ns::ClientReplicaPipeline>();
	view->set_item_catalog(person_catalog());
	const auto body = [&](uint16_t handle, uint16_t type) {
		ns::ClientEntityState &row = staged(*view, handle, type);
		row.lean_angle = 0x01000000;
		row.move_input = static_cast<uint8_t>(w::Entity::kMoveOrderLeanLeft);
		row.pitch_kick_accum = 0x00100000;
		row.arms_dip_ticks = 4;
		row.roll_bam = 0x10000000;
	};
	body(0x0021, kEwepType);
	body(0x0023, kOrg2Type);
	view->tick_lean_decay(kNone);
	view->tick_arms_dip(kNone);
	view->tick_remote_motion(kNone);
	view->tick_lean_ramp();
	const ns::ClientEntityState *ewep = view->state().find(0x0021);
	const ns::ClientEntityState *org2 = view->state().find(0x0023);
	expect(ewep->lean_angle == 0x01000000 && ewep->pitch_kick_accum == 0x00100000 &&
			ewep->arms_dip_ticks == 4 && ewep->torso_roll == 0,
			"the unsplit walk runs no decay, dip, roll or ramp over the item row");
	expect(org2->lean_angle != 0x01000000 && org2->pitch_kick_accum != 0x00100000 &&
			org2->torso_roll != 0, "and every one over the org2 row");
}

// The recoil chase and its PRNG draw belong to the org2 and org1 updates: an item-row person
// draws nothing and keeps its recoil, Yaw and Pitch words, so the shared stream's draws fall to
// the org rows alone.
// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - @0x4B57EB..0x4B5839, PRNG_Next16
//  @0x4B581B; Entity_UpdateInfantryAI @0x4B9910 - @0x4BE7FD..0x4BE84B, PRNG_Next16 @0x4BE82D]
void test_the_recoil_draws_nothing_for_an_item_row() {
	auto view = std::make_unique<ns::ClientReplicaPipeline>();
	view->set_item_catalog(person_catalog());
	staged(*view, 0x0021, kEwepType).recoil_pitch = 0x40000;
	staged(*view, 0x0023, kOrg2Type).recoil_pitch = 0x40000;
	// The org2 row's chase with one draw a tick, and with the item row's draw ahead of it.
	const auto expected = [](int draws_per_tick) {
		uint32_t state = w::World::kMissionPrng16Seed;
		int32_t recoil = 0x40000, heading = 0, pitch = 0;
		for (int tick = 0; tick < 8; ++tick) {
			for (int d = 1; d < draws_per_tick; ++d)
				(void)opennova::io::rotating_prng_next16(state);
			const int32_t random16 = opennova::io::rotating_prng_next16(state);
			const int32_t step = (recoil + 4) >> 3;
			const int32_t half = step >> 1;
			recoil -= half;
			if (recoil <= 0x300) recoil = 0;
			pitch += step >> 3;
			heading += (random16 & 1) == 0 ? half : -half;
		}
		return std::make_pair(heading, pitch);
	};
	expect(expected(1) != expected(2), "the oracle tells the draw counts apart");
	for (int tick = 0; tick < 8; ++tick) view->tick_recoil();
	const ns::ClientEntityState *e = view->state().find(0x0021);
	const ns::ClientEntityState *o = view->state().find(0x0023);
	expect(e->recoil_pitch == 0x40000 && e->heading_bam == 0 && e->pitch_bam == 0,
			"the item row keeps its words");
	expect(std::make_pair(o->heading_bam, o->pitch_bam) == expected(1),
			"the org2 row's chase takes every draw");
}

// Every call is one C2S 0x0F: two persons on one stale ground in one walk ask for the ground
// twice, each ahead of its own body.
// [orig: CNapiNetwork_QueueReliableMessage @0x4C4FA0 -> CNapiNPConnection_QueueMessage
//  @0x628640 (no dedupe); Entity_UpdateTransformAndTurret @0x440CA0 - @0x440DAB, @0x440E29]
void test_every_repair_request_is_queued() {
	auto view = std::make_unique<ns::ClientReplicaPipeline>();
	view->set_remote_motion_mode(true);
	view->set_self_body_split(true);
	view->set_pool1_interleave(true);
	view->set_item_catalog(person_catalog());
	view->set_person_ewep_mover([](ns::ClientEntityState &) { return true; });
	staged(*view, 0x0021, kEwepType).resolved_ground = 0x1004;
	staged(*view, 0x0022, kEwepType).resolved_ground = 0x1004;
	staged(*view, 0x0024, kOrg2Type);
	view->tick_remote_motion(0x0024);
	view->tick_remote_motion_after_self();
	const std::vector<uint16_t> repairs = view->drain_carrier_repair_requests();
	expect(repairs == std::vector<uint16_t>({0x1004, 0x0021, 0x1004, 0x0022}),
			"the ground's request ahead of each body's, both kept");
	expect(view->state().find(0x0021) == nullptr && view->state().find(0x0022) == nullptr,
			"both bodies leave after the walk");
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

// A 0x18 naming the joiner's own handle rebuilds its body's words from the record: the def by
// the type word, Flags, Position and the Yaw / Pitch words, Health the def's healthMax, its Team
// and the record's identity bytes, its ground entity, no mount; the words past +0x2B4 stay.
// [orig: NapiNPClientMsg_FullEntitySpawn @0x433780 - @0x433B69..0x433BBB, @0x433BC7..0x433BDF,
//  @0x433C39, @0x433CDD]
void test_the_full_spawn_rebuilds_the_local_body() {
	Rig r;
	w::PlayerTemplate null_row;
	null_row.type_id = 0;
	null_row.has_item_def = true;
	w::PlayerTemplate plyr;
	plyr.type_id = 0x14B9;
	plyr.has_item_def = true;
	plyr.item_hp = 150;
	r.world.tables.item_rows = {null_row, plyr};
	w::Entity body;
	body.kind = w::EntityKind::Organic;
	body.has_item_def = false; // its own ewep update destroyed it
	body.mounted = true;
	body.spawn_position.x.q16 = 0x00012345;
	w::AiEntity ai;
	ai.inf.vel[0] = 99;
	ns::ClientEntityState self;
	self.type_id = 0x14B9;
	self.x = 1 << 16;
	self.y = 2 << 16;
	self.z = 3 << 16;
	self.heading_bam = 0x20000000;
	self.pitch_bam = 0x01000000;
	self.spawn_entity_flags = 0x101;
	self.team = 2;
	self.net_id = 0x0405;
	self.spawn_anim_slot = 3;
	self.spawn_player_class = 8;
	self.spawn_byte_154 = 5;
	self.target_handle = r.ground.packed;
	self.spawn_sub_type = 2;
	self.spawn_ref_num = 6;
	// Words of the block the record does not store (D-NET-1138).
	body.eye_offset_z = 0xD000;
	body.carry_flags = 0x10100;
	body.display_name = "Lead";
	// The AI entity's homes of block words (+0x90, the idle count +0x148), its AI slot, and a
	// face slot the record's destroy frees (V-ROLL-AQ D1 / D2 / D3).
	ai.body_pitch = 0x00300000;
	ai.inf.idle_counter = 40;
	ai.slot.f[3] = 5;
	body.facial_slot = 0x8000;
	body.facial_checked = true;
	// The local body's look words and the chase's stage, ahead of the record (D-NET-1243).
	ai.inf.target_heading = 0x11110000;
	ai.inf.look_pitch = 0x02220000;
	ai.net_smooth_target[0] = 7 << 16;
	ai.net_smooth_heading = 0x33330000;
	ai.net_smooth_pitch = 0x04440000;
	opennova::inmatch::rebuild_local_body_words(r.world, body, ai, self);
	// D-NET-1243: the record's Yaw and Pitch land on the local body's look words (its input
	// accumulates both), and the model init stamps the chase's stage from Position, the Yaw and
	// the record's Pitch, so the own body's first chase finds no gap: a zeroed stage snapped it to
	// the origin. [orig: NapiNPClientMsg_FullEntitySpawn @0x433780 - Yaw @0x433B8C, Pitch
	//  @0x433BB2, Entity_InitFromModel called @0x433E6C; Entity_InitFromModel @0x40DC30 - +0x234
	//  @0x40E183, +0x240 @0x40E1A7, +0x244 @0x40E1B0; Input_HandleActionBinding_0 @0x4E0420 -
	//  @0x4E0D26, @0x4E0D39; Entity_UpdateInfantryPlayerBody @0x4B40E0 - the gap
	//  @0x4B42C1..0x4B431A, the snap @0x4B4332..0x4B434A]
	expect(ai.inf.target_heading == 0x20000000 && ai.inf.look_pitch == 0x01000000,
			"the record's Yaw and Pitch are the local body's look words");
	expect(ai.net_smooth_target[0] == (1 << 16) && ai.net_smooth_target[1] == (2 << 16) &&
					ai.net_smooth_target[2] == (3 << 16) && ai.net_smooth_heading == 0x20000000 &&
					ai.net_smooth_pitch == 0x01000000,
			"the model init stamps the chase's stage from Position and the angles");
	expect(w::player_body_network_chase(ai, body, /*authority=*/false, /*local=*/true) &&
					body.position.x.q16 == (1 << 16) && body.position.y.q16 == (2 << 16) &&
					body.position.z.q16 == (3 << 16),
			"the own body's first chase finds no gap and stays where the record put it");
	opennova::inmatch::rebuild_local_body_words(r.world, body, ai, self);
	expect(ai.body_pitch == 0 && ai.inf.idle_counter == 0 && ai.slot.f[3] == 0,
			"the memset's AI-entity homes and the destroy's AI slot memset");
	expect(body.facial_slot == 0 && !body.facial_checked,
			"the destroy frees the face slot; the model init allocates a fresh one");
	expect(body.has_item_def && body.item_type_index == 1 && body.item_id == 0x14B9,
			"the record's type word binds the def");
	expect(body.health == 150 && ai.health == 150, "Health is the def's healthMax");
	expect(body.flags == 0x101 && body.position.x.q16 == (1 << 16) && body.position.z.q16 == (3 << 16) &&
			body.heading == 0x20000000 && body.pitch_bam == 0x01000000 && body.roll_bam == 0,
			"Flags, Position and the angles");
	expect(body.team == 2 && body.minimap_net_id == 0x0405 && body.anim_slot == 3 &&
			body.player_class == 8 && body.player_slot_id == 5,
			"the record's identity bytes");
	expect(body.ground_target == r.ground && !body.mounted && ai.inf.vel[0] == 0,
			"its ground entity, no mount, the velocity cleared");
	expect(body.spawn_position.x.q16 == 0x00012345, "the words past +0x2B4 stay");
	expect(body.sub_type == 2, "the record's subType lands on +0x214");
	expect(body.ref_num == 6 && body.display_name.empty(),
			"the record's refNum lands on +0x215; a non-AIData def's Name is the record's empty one");
	expect(body.eye_offset_z == 0 && body.carry_flags == 0,
			"the memset clears the block's words the record does not store");
	// The ground link names the record's slot whether or not it holds a row: a ground the shared
	// destroy removed stays linked (its mark is the stale-ground leg's to read); a handle the
	// decode resolved to none (0xFFFF, or past the pool tables) links nothing.
	// [orig: @0x433C89..0x433CD3, the store @0x433CDD]
	self.target_handle = 0x1006;
	opennova::inmatch::rebuild_local_body_words(r.world, body, ai, self);
	expect(r.world.registry.get(w::EntityHandle{0x1006}) == nullptr &&
			body.ground_target == w::EntityHandle{0x1006}, "an empty slot's handle is linked");
	self.target_handle = kNone;
	opennova::inmatch::rebuild_local_body_words(r.world, body, ai, self);
	expect(!body.ground_target.valid(), "no handle, no link");
}

} // namespace

int main() {
	test_the_full_spawn_rebuilds_the_local_body();
	test_the_respawn_edge_runs_the_reset();
	test_the_walk_runs_the_bound_move_row();
	test_the_split_walk_runs_no_body_legs_over_an_item_row();
	test_the_unsplit_walk_runs_no_body_legs_over_an_item_row();
	test_the_recoil_draws_nothing_for_an_item_row();
	test_every_repair_request_is_queued();
	test_the_update_runs_over_the_rows_words();
	test_the_stale_ground_leg();
	test_the_rider_leg();
	if (failures == 0) std::puts("npruntime_replica_person_ewep: OK");
	return failures == 0 ? 0 : 1;
}
