// The engine's PF_* present-row collectors (runtime/inmatch/present_rows.h,
// ADR 0043 G3): the host/SP pool walk and the joiner's decoded-replica walk,
// pinned over a bare MissionKernel whose registry rows and ClientState are
// authored directly (no boot, no socket, no Godot).
#include <runtime/inmatch/client_replica_emplaced.h>
#include <runtime/inmatch/client_replica_present_projection.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/present_rows.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/replication/client_replica_pipeline.h>
#include <runtime/replication/client_state.h>
#include <runtime/world/entity.h>
#include <runtime/world/angle.h>
#include <runtime/world/present_rows.h>
#include <runtime/world/vehicle_motor.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace w = opennova::world;
namespace im = opennova::inmatch;
namespace nw = opennova;

namespace {

int failures = 0;

bool expect(bool ok, const char *what) {
	if (!ok) {
		std::printf("FAIL: %s\n", what);
		++failures;
	}
	return ok;
}

const float *row_at(const std::vector<float> &rows, size_t i) {
	return rows.data() + i * w::PF_STRIDE;
}

// One organic spawned into pool 0 of a bare kernel's registry at a known slot.
w::Entity *spawn_pool_row(opennova::mission::MissionKernel &kernel, int pool, int slot,
		int32_t item_id) {
	w::EntityRegistry &registry = kernel.world.registry;
	w::Entity seed;
	seed.item_id = item_id;
	seed.alive = true;
	seed.position = {10.0f, 20.0f, 3.0f};
	seed.yaw = 45;
	seed.pitch = 2;
	seed.roll = -3;
	seed.net_stance_bits = 0x02u; // the MoveOrder stance latch bit 9
	seed.section_mask = 0x00010002u;
	seed.spawn_origin = (1u << 24) | 7u; // placed identity: kind 1, index 7
	seed.bms_id = 4242;
	seed.net_id = 99;
	const w::EntityHandle handle = registry.spawn_at(w::EntityHandle::make(pool, slot), seed);
	return registry.get(handle);
}

// The world path: identity, pose, the stance latch, the section mask, the
// dead->alive respawn revision, in registry order.
bool test_world_rows_carry_the_authoritative_record() {
	opennova::mission::MissionKernel kernel;
	kernel.world.registry.configure_pool(1, 64);
	w::Entity *e = spawn_pool_row(kernel, 1, 3, 1291);
	if (!expect(e != nullptr, "the bare registry spawns a pool-1 row")) return false;
	const im::PresentRowsContext context{kernel, nullptr, false};
	im::PoolPresentLifecycleMap lifecycle;
	std::vector<float> rows;
	im::DoorPhaseTable doors;
	im::build_world_present_rows(context, lifecycle, rows, doors);
	bool ok = expect(rows.size() == w::PF_STRIDE, "one row per live registry slot");
	ok = expect(doors.empty(), "a row without door motion publishes no door entry") && ok;
	if (!ok) return false;
	const float *r = row_at(rows, 0);
	ok = expect(static_cast<int>(r[w::PF_TYPE_ID]) == 1291, "PF_TYPE_ID is the item id") && ok;
	ok = expect(static_cast<int>(r[w::PF_WIRE_HANDLE]) == e->handle.packed,
			"PF_WIRE_HANDLE is the packed registry handle") && ok;
	ok = expect(r[w::PF_POS_X] == 10.0f && r[w::PF_POS_Y] == 3.0f && r[w::PF_POS_Z] == -20.0f,
			"the mission (x, y, z) triple presents as (x, z, -y)") && ok;
	ok = expect(static_cast<int>(r[w::PF_YAW_DEG]) == 45 && static_cast<int>(r[w::PF_PITCH_DEG]) == 2 &&
					static_cast<int>(r[w::PF_ROLL_DEG]) == -3,
			"a plain row presents its whole-degree mission eulers") && ok;
	ok = expect(static_cast<int>(r[w::PF_KIND]) == 1 && static_cast<int>(r[w::PF_INDEX]) == 7 &&
					static_cast<int>(r[w::PF_BMS_ID]) == 4242 && static_cast<int>(r[w::PF_NET_ID]) == 99,
			"the placed identity rides kind/index/bms/net") && ok;
	ok = expect(static_cast<int>(r[w::PF_STANCE_BITS]) == 2, "the stance latch is masked to its two bits") && ok;
	ok = expect(r[w::PF_SECTION_MASK_VALID] == 1.0f && static_cast<int>(r[w::PF_SECTION_MASK_LO]) == 2 &&
					static_cast<int>(r[w::PF_SECTION_MASK_HI]) == 1,
			"the section mask splits into its two 16-bit words") && ok;
	ok = expect(r[w::PF_ALIVE] == 1.0f && r[w::PF_HIDDEN] == 0.0f, "alive/hidden mirror the record") && ok;
	ok = expect(r[w::PF_CARRIER_HANDLE] == -1.0f, "a free-standing row publishes the -1 carrier sentinel") && ok;
	ok = expect(r[w::PF_LOCAL_VIEW_SUPPRESSED] == 0.0f, "no local mount: nothing is culled") && ok;
	ok = expect(static_cast<int>(r[w::PF_RESPAWN_REVISION]) == 0, "a fresh row starts at revision 0") && ok;

	// The presentation flag mirrors either authoritative flag store, including
	// existing spawn/teleport copies of a previously set parachute bit.
	ok = expect(r[w::PF_PARACHUTE_DEPLOYED] == 0.0f, "ordinary person has no parachute radius override") && ok;
	e->engine_flags |= w::kEntityFlagParachute;
	im::build_world_present_rows(context, lifecycle, rows, doors);
	ok = expect(row_at(rows, 0)[w::PF_PARACHUTE_DEPLOYED] == 1.0f,
			"the engine flag reaches the person projection row") && ok;
	e->engine_flags &= ~w::kEntityFlagParachute;
	e->flags |= w::kEntityFlagParachute;
	im::build_world_present_rows(context, lifecycle, rows, doors);
	ok = expect(row_at(rows, 0)[w::PF_PARACHUTE_DEPLOYED] == 1.0f,
			"the organic low-byte flag also reaches the projection row") && ok;
	e->flags &= ~w::kEntityFlagParachute;
	im::build_world_present_rows(context, lifecycle, rows, doors);
	ok = expect(row_at(rows, 0)[w::PF_PARACHUTE_DEPLOYED] == 0.0f,
			"clearing both stores removes the radius override") && ok;

	// The dead->alive edge of the authoritative flags bumps the revision once;
	// a re-spawned slot (new registry_spawn_id) forgets the old edge state.
	e->flags |= w::kEntityFlagDead;
	im::build_world_present_rows(context, lifecycle, rows, doors);
	e->flags &= ~static_cast<uint32_t>(w::kEntityFlagDead);
	im::build_world_present_rows(context, lifecycle, rows, doors);
	ok = expect(static_cast<int>(row_at(rows, 0)[w::PF_RESPAWN_REVISION]) == 1,
			"a dead->alive edge bumps the respawn revision") && ok;
	im::build_world_present_rows(context, lifecycle, rows, doors);
	ok = expect(static_cast<int>(row_at(rows, 0)[w::PF_RESPAWN_REVISION]) == 1,
			"a steady alive row keeps its revision") && ok;
	return ok;
}

nw::OrganicSpawnRecord organic_record(uint16_t slot, uint16_t type_id) {
	nw::OrganicSpawnRecord rec;
	rec.slot_id = slot;
	rec.has_body = true;
	rec.item_type_id = type_id;
	rec.pos_x = 5 << 16;
	rec.pos_y = 6 << 16;
	rec.pos_z = 7 << 16;
	return rec;
}

// The joiner path: one row per decoded replica through the canonical
// projection, the self echo left blank, the transition pulses left for the
// caller to consume.
bool test_full_spawn_parachute_state_reaches_player_and_infantry_rows() {
	for (const bool player : {false, true}) {
		opennova::mission::MissionKernel kernel;
		im::ClientRuntime runtime("ParachuteProjection");
		opennova::replication::ClientReplicaPipeline pipeline;
		nw::FullEntitySpawnRecord record;
		record.slot_id = 0x0010u;
		record.item_type = 3;
		record.item_type_id = 0x1410u;
		record.minimap_flags = 0x20u | (player ? 0x100u : 0u);
		pipeline.apply(nw::s2c::FULL_ENTITY_SPAWN, nw::encode_full_entity_spawn(record));
		runtime.state() = pipeline.state();
		if (!expect(runtime.state().entities.size() == 1,
				"a full-slot person record creates its received state")) return false;
		if (!expect(runtime.state().entities[0].cls ==
				(player ? nw::EntityClass::Player : nw::EntityClass::Infantry),
				"full-slot flags distinguish players from NPC infantry")) return false;
		const im::PresentRowsContext context{kernel, &runtime, true};
		std::vector<float> rows;
		im::DoorPhaseTable doors;
		im::PoolPresentLifecycleMap lifecycle;
		im::build_client_replica_present_rows(context, lifecycle, rows, doors);
		if (!expect(rows.size() == w::PF_STRIDE &&
				row_at(rows, 0)[w::PF_PARACHUTE_DEPLOYED] == 1.0f,
				"received parachute state reaches both player and NPC projection rows")) return false;
		runtime.state().entities[0].rm_entity_flags &= ~w::kEntityFlagParachute;
		im::build_client_replica_present_rows(context, lifecycle, rows, doors);
		if (!expect(row_at(rows, 0)[w::PF_PARACHUTE_DEPLOYED] == 0.0f,
				"a received-state clear removes the projection override")) return false;
	}
	return true;
}

bool test_replica_rows_project_the_decoded_state_and_keep_the_pulses() {
	opennova::mission::MissionKernel kernel;
	im::ClientRuntime runtime("PresentRows");
	nw::OrganicSpawnBatch batch;
	batch.records.push_back(organic_record(0x0010u, 0x1410u));
	batch.records.push_back(organic_record(0x0011u, 0x1411u));
	batch.entity_count = 2;
	runtime.state() = opennova::replication::ClientState{};
	opennova::replication::ClientReplicaPipeline pipeline;
	pipeline.apply(nw::s2c::ENTITY_SPAWN_BATCH, nw::encode_organic_spawn_batch(batch));
	runtime.state() = pipeline.state();
	if (!expect(runtime.state().entities.size() == 2, "the fixture decoded two organics")) return false;
	runtime.state().entities[1].anim_state_pulse = 1; // a live transition pulse

	const im::PresentRowsContext context{kernel, &runtime, true};
	std::vector<float> rows;
	im::DoorPhaseTable doors;
	im::PoolPresentLifecycleMap lifecycle;
	im::build_client_replica_present_rows(context, lifecycle, rows, doors);
	bool ok = expect(rows.size() == 2 * w::PF_STRIDE, "one row per decoded replica");
	ok = expect(doors.empty(), "a joiner's decoded rows carry no door entries") && ok;
	if (!ok) return false;
	const float *first = row_at(rows, 0);
	ok = expect(static_cast<int>(first[w::PF_TYPE_ID]) == 0x1410 &&
					static_cast<int>(first[w::PF_WIRE_HANDLE]) == 0x0010,
			"the projection carries the decoded identity") && ok;
	ok = expect(first[w::PF_POS_X] == 5.0f && first[w::PF_POS_Y] == 7.0f && first[w::PF_POS_Z] == -6.0f,
			"the decoded 16.16 pose presents as (x, z, -y)") && ok;
	// A header-only joiner has no authored identity behind the handle: the row
	// keeps the initializer's sentinels for kind/index/bms/net.
	float blank[w::PF_STRIDE];
	im::initialize_client_replica_present_row(blank);
	ok = expect(first[w::PF_KIND] == blank[w::PF_KIND] && first[w::PF_INDEX] == blank[w::PF_INDEX] &&
					first[w::PF_BMS_ID] == blank[w::PF_BMS_ID] && first[w::PF_NET_ID] == blank[w::PF_NET_ID],
			"a header-only joiner's row carries no authored identity") && ok;
	ok = expect(runtime.state().entities[1].anim_state_pulse == 1,
			"the builder leaves the transition pulses for the caller to consume") && ok;
	ok = expect(first[w::PF_DEATH_CTRL] == 65535.0f,
			"a wire-only row carries no corpse timer: the live DEATH value") && ok;
	return ok;
}

} // namespace

bool test_vehicle_suspension_reaches_present_rows() {
	opennova::mission::MissionKernel kernel;
	kernel.world.registry.configure_pool(1, 16);
	w::Entity *e = spawn_pool_row(kernel, 1, 3, 1291);
	if (!expect(e != nullptr, "vehicle row exists"))
		return false;
	w::VehicleTraits traits;
	traits.physics = 1;
	kernel.world.vehicles.traits.set(1291, traits);
	e->veh.wheel_comp[0] = 100;
	e->veh.wheel_comp[1] = 200;
	e->veh.wheel_comp[2] = 500;
	e->veh.wheel_comp[3] = 1000;
	im::PoolPresentLifecycleMap lifecycle;
	std::vector<float> rows;
	im::DoorPhaseTable doors;
	im::build_world_present_rows({ kernel, nullptr, false }, lifecycle, rows, doors);
	if (!expect(rows.size() == w::PF_STRIDE, "one vehicle presentation row"))
		return false;
	const int expected[] = { 100, 200, 300, 600, 1000, 500 };
	bool ok = expect(rows[w::PF_VEHICLE_MOTION_VALID] == 1, "vehicle controls valid");
	for (int i = 0; i < 6; ++i)
		ok = expect(rows[w::PF_VEHICLE_TIRE00 + i] == expected[i],
					 "suspension compression reaches the model snapshot") &&
				ok;
	traits.render_family = w::VehicleRenderFamily::Tank;
	kernel.world.vehicles.traits.set(1291, traits);
	e->veh.track_phase[0] = -65536;
	e->veh.track_phase[1] = 0x12340000;
	im::build_world_present_rows({ kernel, nullptr, false }, lifecycle, rows, doors);
	ok = expect((static_cast<int>(rows[w::PF_VEHICLE_CTRL_MASK]) & w::VC_TRACKS) != 0 &&
						 (static_cast<int>(rows[w::PF_VEHICLE_CTRL_MASK]) & w::VC_TIRES) == 0,
				 "tank render selects tracks without ground suspension ownership") &&
			ok;
	ok = expect(rows[w::PF_VEHICLE_TRACK_LEFT] == 65535 &&
						 rows[w::PF_VEHICLE_TRACK_RIGHT] == 0x1234,
				 "both track phases reach the model snapshot") &&
			ok;
	return ok;
}

// A motor-driven hull publishes the motor's BAM pitch/roll, not their
// whole-degree mirrors: the drawn hull (and the cockpit drawn at its transform)
// then sits in the same frame as entity_placement_matrix, which places the
// carrier-owned first-person eye. A 0.49 degree mirror error over the 1.83-unit
// camera arm is the 1.6 cm the driver's eye sat off its cockpit.
bool test_vehicle_rows_publish_the_motor_attitude_unrounded() {
	opennova::mission::MissionKernel kernel;
	kernel.world.registry.configure_pool(1, 16);
	w::Entity *e = spawn_pool_row(kernel, 1, 3, 1291);
	if (!expect(e != nullptr, "vehicle row exists"))
		return false;
	// Unseeded: the whole-degree mission mirrors are all a row carries.
	e->pitch = 3;
	e->roll = -2;
	im::PoolPresentLifecycleMap lifecycle;
	std::vector<float> rows;
	im::DoorPhaseTable doors;
	im::build_world_present_rows({ kernel, nullptr, false }, lifecycle, rows, doors);
	bool ok = expect(rows[w::PF_PITCH_DEG] == 3.0f && rows[w::PF_ROLL_DEG] == -2.0f,
			"an unseeded row publishes the mission mirrors");
	// Seeded: 2.49 / -1.51 degrees round to 2 / -2 in the mirrors.
	const double bam_per_degree = 4294967296.0 / 360.0;
	e->veh.yaw_seeded = true;
	e->veh.air_pitch_bam = static_cast<int32_t>(2.49 * bam_per_degree);
	e->veh.air_roll_bam = static_cast<int32_t>(-1.51 * bam_per_degree);
	e->pitch = 2;
	e->roll = -2;
	im::build_world_present_rows({ kernel, nullptr, false }, lifecycle, rows, doors);
	ok = expect(std::abs(rows[w::PF_PITCH_DEG] - 2.49f) < 1e-4f &&
					 std::abs(rows[w::PF_ROLL_DEG] + 1.51f) < 1e-4f,
				 "a motor-seeded row publishes the BAM attitude") &&
			ok;
	ok = expect(std::abs(im::pool_present_pitch_deg(*e) - 2.49) < 1e-6 &&
					 std::abs(im::pool_present_roll_deg(*e) + 1.51) < 1e-6,
				 "the shared helpers read the same pair") &&
			ok;
	return ok;
}

// Both pool and client-backed collectors must retain an attached gun's live
// frame. The authored integer mirrors intentionally differ from that frame.
bool test_attached_rows_retain_subdegree_frame() {
    opennova::mission::MissionKernel kernel;
    kernel.world.registry.configure_pool(1, 4);
    kernel.world.registry.configure_pool(2, 4);
    w::Entity *parent = spawn_pool_row(kernel, 1, 0, 1291);
    w::Entity *gun = spawn_pool_row(kernel, 2, 0, 1871);
    if (!expect(parent && gun, "carrier and attached gun spawn")) return false;
    gun->emplacement_parent = parent->handle;
    gun->emplacement_parent_spawn_id = parent->registry_spawn_id;
    gun->emplacement_pose_metadata_resolved = true;
    gun->veh.yaw_seeded = true;
    gun->veh.yaw_bam = w::bam_heading_from_mission_yaw_deg(33.125);
    gun->veh.air_pitch_bam = w::bam_from_degrees_wrapped(2.49);
    gun->veh.air_roll_bam = w::bam_from_degrees_wrapped(-1.51);
    gun->yaw = 33; gun->pitch = 2; gun->roll = -2;
    im::PoolPresentLifecycleMap lifecycle;
    std::vector<float> rows;
    im::DoorPhaseTable doors;
    const auto check_gun = [&]() {
        for (size_t i = 0; i * w::PF_STRIDE < rows.size(); ++i) {
            const float *r = row_at(rows, i);
            if (static_cast<uint16_t>(r[w::PF_WIRE_HANDLE]) != gun->handle.packed) continue;
            return expect(std::abs(r[w::PF_YAW_DEG] - 33.125f) < 1e-4f &&
                          std::abs(r[w::PF_PITCH_DEG] - 2.49f) < 1e-4f &&
                          std::abs(r[w::PF_ROLL_DEG] + 1.51f) < 1e-4f,
                          "attached gun retains the sub-degree bone attitude");
        }
        return expect(false, "attached gun is presented");
    };
    im::build_world_present_rows({kernel, nullptr, false}, lifecycle, rows, doors);
    bool ok = check_gun();
    im::ClientRuntime runtime("AttachedRows");
    opennova::replication::ClientEntityState decoded;
    decoded.handle = gun->handle.packed;
    decoded.type_id = 1871;
    decoded.cls = opennova::EntityClass::NoNetworkCallback;
    // The 0x0D form: the carrier rides the TARGET (groundEntity), the parent is
    // the gunner back-reference (+0x170) [orig: serialize_entity_pool_to_packet_0
    // +0x170 @0x503BC9, +0x28 @0x503C22].
    decoded.target_handle = parent->handle.packed;
    decoded.parent_handle = 0x0003;
    runtime.state().upsert(decoded.handle) = decoded;
    // The listen host enriches its client-backed rows from the authoritative
    // pool. Joiners use decoded carriers/model assets, never this authority row.
    im::build_client_replica_present_rows({kernel, &runtime, false}, lifecycle, rows, doors);
    ok = check_gun() && ok;
    return ok;
}

bool test_door_phases_reach_present_rows() {
    opennova::mission::MissionKernel kernel;
    kernel.world.registry.configure_pool(2, 1);
    w::Entity *e = spawn_pool_row(kernel, 2, 0, 1998);
    if (!expect(e != nullptr, "door row spawns")) return false;
    e->door_motion = e->door_event = true;
    e->door_count = 1;
    kernel.world.doors.initialize(*e, 65536, 0);
    kernel.world.doors.command(kernel.world, *e, 7);
    kernel.world.doors.tick(kernel.world);
	im::PoolPresentLifecycleMap lifecycle;
	std::vector<float> rows;
	im::DoorPhaseTable doors;
	im::build_world_present_rows({ kernel, nullptr, false }, lifecycle, rows, doors);
	if (!expect(rows.size() == w::PF_STRIDE, "one door row")) return false;
	bool ok = expect(row_at(rows, 0)[w::PF_DOOR_COUNT] == 1 &&
					 doors == im::DoorPhaseTable{ 0, 1, 65536 },
			"open phase 65536 rides the door side table exactly (row 0, one phase)");
	e->door_motion = false;
	im::build_world_present_rows({ kernel, nullptr, false }, lifecycle, rows, doors);
	ok = expect(row_at(rows, 0)[w::PF_DOOR_COUNT] == 0 && doors.empty(),
				 "a missing door movement callback releases phase ownership") &&
			ok;
	return ok;
}

// The joiner projection: the materialized local row behind a decoded handle
// is a real DoorSystem row (ticked and contact-driven on every peer), and its
// Q16 phases ride the same PF_DOOR_COUNT + side table the authority's
// collector builds (D-DOOR-4).
bool test_joiner_door_phases_reach_present_rows() {
	opennova::mission::MissionKernel kernel;
	kernel.world.registry.configure_pool(2, 2);
	im::ClientRuntime runtime("DoorRows");
	w::Entity *e = spawn_pool_row(kernel, 2, 0, 1998);
	if (!expect(e != nullptr, "joiner door row spawns")) return false;
	e->door_motion = e->door_event = true;
	e->door_count = 1;
	// Half-open per tick: one tick lands mid-motion, the second clamps open.
	kernel.world.doors.initialize(*e, 32768, 0);
	kernel.world.doors.command(kernel.world, *e, 7);
	kernel.world.doors.tick(kernel.world);
	opennova::replication::ClientEntityState decoded;
	decoded.handle = e->handle.packed;
	decoded.type_id = 1998;
	decoded.cls = opennova::EntityClass::NoNetworkCallback;
	runtime.state().upsert(decoded.handle) = decoded;
	const im::PresentRowsContext context{ kernel, &runtime, true };
	im::PoolPresentLifecycleMap lifecycle;
	std::vector<float> rows;
	im::DoorPhaseTable doors;
	im::build_client_replica_present_rows(context, lifecycle, rows, doors);
	if (!expect(rows.size() == w::PF_STRIDE, "one joiner door row")) return false;
	bool ok = expect(row_at(rows, 0)[w::PF_DOOR_COUNT] == 1 &&
					 doors == im::DoorPhaseTable{ 0, 1, 32768 },
			"a joiner's local door row publishes its mid-motion phase exactly (row 0, one phase)");
	kernel.world.doors.tick(kernel.world);
	im::build_client_replica_present_rows(context, lifecycle, rows, doors);
	ok = expect(row_at(rows, 0)[w::PF_DOOR_COUNT] == 1 &&
					 doors == im::DoorPhaseTable{ 0, 1, 65536 },
				 "the fully open 65536 rides the joiner's side table exactly") &&
			ok;
	// The decoded row is only this local row's door source when the types agree.
	runtime.state().upsert(decoded.handle).type_id = 1999;
	im::build_client_replica_present_rows(context, lifecycle, rows, doors);
	ok = expect(row_at(rows, 0)[w::PF_DOOR_COUNT] == 0 && doors.empty(),
				 "a type-mismatched local row publishes no door entry") &&
			ok;
	runtime.state().upsert(decoded.handle).type_id = 1998;
	e->door_motion = false;
	im::build_client_replica_present_rows(context, lifecycle, rows, doors);
	ok = expect(row_at(rows, 0)[w::PF_DOOR_COUNT] == 0 && doors.empty(),
				 "a missing door movement callback releases phase ownership on the joiner") &&
			ok;
	return ok;
}

// The joiner projection publishes a replica vehicle's controls from its local
// twin, whose client mover advances the same tracks, wheels and springs: the
// tank render callback reads those live fields on every peer.
// [orig: HUD_CacheEntityDebugStats @0x449C10, track words @0x449C3C..0x449C69;
//  the client mover's track phase Entity_UpdateTankVehiclePhysics @0x489F98 /
//  @0x489FA0]
bool test_joiner_vehicle_motion_controls_reach_present_rows() {
	opennova::mission::MissionKernel kernel;
	kernel.world.registry.configure_pool(1, 2);
	im::ClientRuntime runtime("TankRows");
	w::Entity *e = spawn_pool_row(kernel, 1, 0, 1296);
	if (!expect(e != nullptr, "joiner tank row spawns")) return false;
	w::VehicleTraits traits;
	traits.family = w::VehicleFamily::Tank;
	traits.render_family = w::VehicleRenderFamily::Tank;
	kernel.world.vehicles.traits.set(1296, traits);
	e->veh.track_phase[0] = 0x12340000;
	e->veh.track_phase[1] = 0x56780000;
	opennova::replication::ClientEntityState decoded;
	decoded.handle = e->handle.packed;
	decoded.type_id = 1296;
	decoded.cls = opennova::EntityClass::Vehicle;
	runtime.state().upsert(decoded.handle) = decoded;
	const im::PresentRowsContext context{ kernel, &runtime, true };
	im::PoolPresentLifecycleMap lifecycle;
	std::vector<float> rows;
	im::DoorPhaseTable doors;
	im::build_client_replica_present_rows(context, lifecycle, rows, doors);
	if (!expect(rows.size() == w::PF_STRIDE, "one joiner tank row")) return false;
	const float *row = row_at(rows, 0);
	return expect(row[w::PF_VEHICLE_MOTION_VALID] == 1.0f &&
					(static_cast<int>(row[w::PF_VEHICLE_CTRL_MASK]) & w::VC_TRACKS) != 0 &&
					row[w::PF_VEHICLE_TRACK_LEFT] == float(0x1234) &&
					row[w::PF_VEHICLE_TRACK_RIGHT] == float(0x5678),
			"a joiner's tank row publishes its twin's track phases");
}

bool test_joiner_palm_source_and_local_fragment() {
    opennova::mission::MissionKernel kernel;
    kernel.world.registry.configure_pool(2, 4);
    im::ClientRuntime runtime("PalmRows");
    auto *source = spawn_pool_row(kernel, 2, 0, 812);
    source->kind = w::EntityKind::Building;
    source->palm_sections = true; source->palm_state = 2; source->section_mask = 0;
    source->pitch = source->roll = 0; source->yaw = 90;
    opennova::replication::ClientEntityState decoded;
    decoded.handle = source->handle.packed;
    decoded.type_id = 812;
    decoded.cls = opennova::EntityClass::NoNetworkCallback;
    runtime.state().upsert(decoded.handle) = decoded;
    w::Entity piece;
    piece.kind = w::EntityKind::Building; piece.item_id = 900;
    piece.position = {10, 20, 7}; piece.yaw = 90;
    piece.palm_sections = true; piece.item_section_piece = true; piece.palm_state = 17;
    piece.alive = false;
    const auto h = kernel.world.registry.spawn(2, piece);
    w::ItemDeathTraits traits;
    traits.model_pivots_q16 = {{{0, 0, 2*65536}}, {{0, 0, 4*65536}}};
    kernel.world.tables.item_death_traits.set(900, traits);
    im::PresentRowsContext context{kernel, &runtime, true};
    im::PoolPresentLifecycleMap lifecycle;
    std::vector<float> rows; im::DoorPhaseTable doors;
    im::build_client_replica_present_rows(context, lifecycle, rows, doors);
    bool ok = expect(rows.size() == 2 * w::PF_STRIDE, "joiner presents its local fragment");
    if (!ok) return false;
    ok = expect(row_at(rows, 0)[w::PF_SECTION_MASK_LO] == 0x1C &&
            row_at(rows, 0)[w::PF_ALIVE] == 1, "source uses its partial section state") && ok;
    ok = expect(row_at(rows, 1)[w::PF_WIRE_HANDLE] == h.packed &&
            row_at(rows, 1)[w::PF_POS_Y] == 3 &&
            row_at(rows, 1)[w::PF_SECTION_MASK_LO] == 0x3B,
            "fragment retains its CXLT pivot and one visible section") && ok;
    // Only the fragment rides the pool-row writer: the decoded source keeps
    // its wire lifecycle and never enters the shared pool lifecycle map.
    ok = expect(lifecycle.find(h.packed) != lifecycle.end() &&
            lifecycle.find(source->handle.packed) == lifecycle.end() &&
            doors.empty(),
            "joiner fragment append touches only the fragment's lifecycle entry") && ok;
    source->palm_sections = false;
    source->engine_flags |= w::kEntityFlagHusk;
    source->spawned_piece_mask = 12;
    source->destroy_phases_q16 = {123,456,789,1024,32768,65536};
    traits.death_class = w::ItemDeathClass::kTower;
    kernel.world.tables.item_death_traits.set(812, traits);
    kernel.world.registry.get(h)->engine_flags |= w::kEntityFlagHusk;
    im::build_client_replica_present_rows(context, lifecycle, rows, doors);
    ok = expect(row_at(rows, 0)[w::PF_HUSK] == 1 &&
            row_at(rows, 0)[w::PF_SECTION_MASK_LO] == 12 &&
            row_at(rows, 1)[w::PF_HUSK] == 1,
            "joiner keeps partial tower masks and a fragment born as a husk") && ok;
    ok = expect(row_at(rows, 0)[w::PF_OBJECT_DESTROY] == 123 &&
            row_at(rows, 0)[w::PF_OBJECT_DESTROY05] == 65536,
            "joiner publishes exact native destruction CTRL words") && ok;
    kernel.world.registry.despawn(h);
    im::build_client_replica_present_rows(context, lifecycle, rows, doors);
    ok = expect(rows.size() == w::PF_STRIDE, "retired fragment leaves the present rows") && ok;
    return ok;
}
// The org0 skin bone-callback's DEATH register rides the authoritative organic
// row (world::death_ctrl_register_value): live 0xFFFF, the 186-tick ramp once
// dead, 0 for the last 62 ticks; a non-organic row reads the live value.
// A joiner runs no brains: the tank turret child's class update publishes its
// gun yaw word on the hull's replica row, and the hull row carries it as the
// tank render callback's VEHICLE_GUNYAW, held once the turret stops moving.
// A child that is no ewep class publishes nothing.
// [orig: Entity_UpdateTransformAndTurret GROUND @0x440F70..0x440F8A;
//  HUD_CacheEntityDebugStats @0x449ECF..0x449EE2]
bool test_joiner_hull_gun_words_follow_the_turret_child() {
	opennova::mission::MissionKernel kernel;
	kernel.world.registry.configure_pool(1, 4);
	im::ClientRuntime runtime("TankRows");
	w::Entity *hull = spawn_pool_row(kernel, 1, 0, 700);
	w::Entity *turret = spawn_pool_row(kernel, 1, 1, 701);
	if (!expect(hull != nullptr && turret != nullptr, "tank rows spawn")) return false;
	turret->emplaced_update = true;
	turret->primary_weapon = "WPN_TURRET";
	kernel.world.tables.weapons.entries.resize(1);
	kernel.world.tables.weapons.entries[0].valid = true;
	kernel.world.tables.weapons.entries[0].name = "WPN_TURRET";
	w::VehicleTraits traits;
	traits.render_family = w::VehicleRenderFamily::Tank;
	traits.family = w::VehicleFamily::Tank;
	kernel.world.vehicles.traits.set(700, traits);
	opennova::mission::ItemSeatSpec spec;
	spec.type_id = 700;
	opennova::mission::ItemEmplacementAttachmentSpec attachment;
	attachment.child_type_id = 701;
	attachment.anchor_found = true;
	attachment.anchor.bone_index = 3;
	attachment.anchor_subobject = 0;
	spec.emplacement_attachments.push_back(attachment);
	kernel.seat_specs.push_back(spec);
	opennova::replication::ClientEntityState hull_row;
	hull_row.handle = hull->handle.packed;
	hull_row.type_id = 700;
	hull_row.cls = opennova::EntityClass::NoNetworkCallback;
	runtime.state().upsert(hull_row.handle) = hull_row;
	opennova::replication::ClientEntityState turret_row;
	turret_row.handle = turret->handle.packed;
	turret_row.type_id = 701;
	turret_row.cls = opennova::EntityClass::NoNetworkCallback;
	turret_row.parent_handle = hull_row.handle;
	turret_row.emplaced_gun_yaw_word = 0x1234;
	runtime.state().upsert(turret_row.handle) = turret_row;
	im::tick_replica_emplaced_channels(runtime.state(), kernel.seat_specs, kernel.world, 0xFFFF);
	const im::PresentRowsContext context{ kernel, &runtime, true };
	im::PoolPresentLifecycleMap lifecycle;
	std::vector<float> rows;
	im::DoorPhaseTable doors;
	im::build_client_replica_present_rows(context, lifecycle, rows, doors);
	if (!expect(rows.size() == 2 * w::PF_STRIDE, "a hull row and a turret row")) return false;
	const float *hull_present = row_at(rows, 0);
	bool ok = expect(hull_present[w::PF_VEHICLE_MOTION_VALID] == 1.0f &&
					(static_cast<uint32_t>(hull_present[w::PF_VEHICLE_CTRL_MASK]) &
							w::VC_VEHICLE_GUN) != 0 &&
					hull_present[w::PF_VEHICLE_GUN_YAW] == static_cast<float>(0x1234),
			"the turret's yaw word drives the joiner hull's VEHICLE_GUNYAW");
	runtime.state().find(turret_row.handle)->parent_handle = w::EntityHandle::kInvalid;
	im::tick_replica_emplaced_channels(runtime.state(), kernel.seat_specs, kernel.world, 0xFFFF);
	im::build_client_replica_present_rows(context, lifecycle, rows, doors);
	ok = expect(row_at(rows, 0)[w::PF_VEHICLE_GUN_YAW] == static_cast<float>(0x1234),
			"the hull keeps the last published word, like the brain") && ok;

	opennova::mission::MissionKernel plain;
	plain.world.registry.configure_pool(1, 4);
	im::ClientRuntime plain_runtime("PlainRows");
	w::Entity *plain_hull = spawn_pool_row(plain, 1, 0, 700);
	w::Entity *plain_child = spawn_pool_row(plain, 1, 1, 701);
	plain_child->primary_weapon = "WPN_TURRET";
	plain.world.tables.weapons.entries = kernel.world.tables.weapons.entries;
	plain.world.vehicles.traits.set(700, traits);
	plain.seat_specs.push_back(spec);
	hull_row.handle = plain_hull->handle.packed;
	turret_row.handle = plain_child->handle.packed;
	turret_row.parent_handle = hull_row.handle;
	plain_runtime.state().upsert(hull_row.handle) = hull_row;
	plain_runtime.state().upsert(turret_row.handle) = turret_row;
	im::tick_replica_emplaced_channels(plain_runtime.state(), plain.seat_specs, plain.world, 0xFFFF);
	const im::PresentRowsContext plain_context{ plain, &plain_runtime, true };
	im::PoolPresentLifecycleMap plain_lifecycle;
	im::build_client_replica_present_rows(plain_context, plain_lifecycle, rows, doors);
	// The hull still publishes its own motion controls (the joiner twin's
	// tracks and wheels); only the gun bit stays clear.
	ok = expect((static_cast<uint32_t>(row_at(rows, 0)[w::PF_VEHICLE_CTRL_MASK]) &
					w::VC_VEHICLE_GUN) == 0,
			"a child without the ewep class update publishes no gun words") && ok;
	return ok;
}

bool test_death_ctrl_register_reaches_present_rows() {
	opennova::mission::MissionKernel kernel;
	kernel.world.registry.configure_pool(0, 4);
	kernel.world.registry.configure_pool(1, 4);
	w::Entity *body = spawn_pool_row(kernel, 0, 1, 1337);
	w::Entity *vehicle = spawn_pool_row(kernel, 1, 2, 1291);
	if (!expect(body != nullptr && vehicle != nullptr, "the organic and vehicle rows spawn"))
		return false;
	body->kind = w::EntityKind::Organic;
	im::PoolPresentLifecycleMap lifecycle;
	std::vector<float> rows;
	im::DoorPhaseTable doors;
	const auto death_of = [&](int32_t type_id) -> float {
		for (size_t i = 0; i * w::PF_STRIDE < rows.size(); ++i)
			if (static_cast<int32_t>(row_at(rows, i)[w::PF_TYPE_ID]) == type_id)
				return row_at(rows, i)[w::PF_DEATH_CTRL];
		return -1.0f;
	};
	im::build_world_present_rows({ kernel, nullptr, false }, lifecycle, rows, doors);
	if (!expect(rows.size() == 2 * w::PF_STRIDE, "one row per live pool slot")) return false;
	bool ok = expect(death_of(1337) == 65535.0f, "a live body reads 0xFFFF");
	ok = expect(death_of(1291) == 65535.0f, "a vehicle row reads the live value") && ok;
	body->flags |= w::kEntityFlagDead;
	body->corpse_timer = 300;
	im::build_world_present_rows({ kernel, nullptr, false }, lifecycle, rows, doors);
	ok = expect(death_of(1337) == 65535.0f, "a corpse still above 248 reads 0xFFFF") && ok;
	body->corpse_timer = 100;
	im::build_world_present_rows({ kernel, nullptr, false }, lifecycle, rows, doors);
	ok = expect(static_cast<int>(death_of(1337)) == (38 << 16) / 186,
			"a corpse at timer 100 reads (38 << 16) / 186") && ok;
	body->corpse_timer = 10;
	im::build_world_present_rows({ kernel, nullptr, false }, lifecycle, rows, doors);
	ok = expect(death_of(1337) == 0.0f, "the last 62 ticks hold 0") && ok;
	return ok;
}

int main() {
    test_attached_rows_retain_subdegree_frame();
    test_joiner_palm_source_and_local_fragment();
    test_door_phases_reach_present_rows();
    test_joiner_door_phases_reach_present_rows();
	test_vehicle_suspension_reaches_present_rows();
	bool ok = true;
	ok = test_vehicle_rows_publish_the_motor_attitude_unrounded() && ok;
	ok = test_world_rows_carry_the_authoritative_record() && ok;
	ok = test_full_spawn_parachute_state_reaches_player_and_infantry_rows() && ok;
	ok = test_replica_rows_project_the_decoded_state_and_keep_the_pulses() && ok;
	ok = test_death_ctrl_register_reaches_present_rows() && ok;
	ok = test_joiner_vehicle_motion_controls_reach_present_rows() && ok;
	ok = test_joiner_hull_gun_words_follow_the_turret_child() && ok;
	if (!ok || failures != 0) {
		std::printf("present_rows_test: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("present_rows_test: ok\n");
	return 0;
}
