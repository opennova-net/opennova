// The engine's PF_* present-row collectors (runtime/inmatch/present_rows.h,
// ADR 0043 G3): the host/SP pool walk and the joiner's decoded-replica walk,
// pinned over a bare MissionKernel whose registry rows and ClientState are
// authored directly (no boot, no socket, no Godot).
#include <runtime/inmatch/client_replica_present_projection.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/present_rows.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/replication/client_replica_pipeline.h>
#include <runtime/replication/client_state.h>
#include <runtime/world/entity.h>
#include <runtime/world/present_rows.h>
#include <runtime/world/vehicle_motor.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>

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
int main() {
    test_joiner_palm_source_and_local_fragment();
    test_door_phases_reach_present_rows();
	test_vehicle_suspension_reaches_present_rows();
	bool ok = true;
	ok = test_world_rows_carry_the_authoritative_record() && ok;
	ok = test_replica_rows_project_the_decoded_state_and_keep_the_pulses() && ok;
	if (!ok || failures != 0) {
		std::printf("present_rows_test: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("present_rows_test: ok\n");
	return 0;
}
