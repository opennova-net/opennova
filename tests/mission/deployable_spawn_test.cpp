// D-NET-332: a vehicle bay's deployable. The authority's C2S 0x40 handler spawns the
// bay's group vehicle at the bay's "boat" (else "helo") userpoint through the bay's euler
// placement, else 2.0 over the bay, with the bay's attitude words in the block, stamps the
// slot team and mounts the requester at the vehicle def's control bone; the kernel's
// spawner binds the template (the block's position and Yaw word, pitch and roll 0, SSN
// 12000, the team, the deployable bit), its health ceiling, the AI slot with the def's
// default_aip (16 bytes) and the class brain for an AI-class row, and refuses a type no
// row carries. The rows and the bay are the test's own.
// [orig: NapiNPServerMsg_HandleVehicleSpawnRequest @0x51C4C0 (the block @0x51C606..0x51C630,
//  the finds @0x51C63D / @0x51C654, the transform @0x51C668 / @0x51C678, the raise @0x51C682,
//  the spawn @0x51C69E, the team @0x51C6BD, the control bone @0x51C7EB, the attach
//  @0x51C7EF); Entity_SpawnDeployable @0x51C2B0 (@0x51C317..0x51C3CB, @0x51C42F, @0x51C454,
//  @0x51C467..0x51C484, @0x51C492); Entity_CloneFromTemplateToPool1 @0x439830 (@0x439835)]
#include "npruntime/conn_fixture.h"
#include <formats/aip/aip.h>
#include <formats/def/def.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_vehicle_spawn.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/world/collision.h>
#include <runtime/world/world.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

using namespace opennova;
namespace w = opennova::world;

namespace {

int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %d: %s\n", __LINE__, #c); ++failures; } } while (0)

void test_the_spawner_binds_the_template() {
	std::array<def::DefItemDef, 2> rows{};
	rows[0].id = mission::kItemIdOffset + 1; // the first row: its index 0 is the clone's refusal
	rows[0].type = 1;
	rows[1].id = mission::kItemIdOffset + 1400;
	rows[1].type = 1;
	rows[1].attrib = w::kItemAttribAIData;
	rows[1].hp = 600;
	std::strcpy(rows[1].ai_function, "cveh");
	std::strcpy(rows[1].default_aip, "Deployable_Boat_Profile");
	def::DefItemsFile items{rows.data(), rows.size()};
	auto kernel = std::make_unique<mission::MissionKernel>();
	kernel->set_items_table(&items);
	for (int pool = 0; pool < 3; ++pool) kernel->world.registry.configure_pool(pool, 8);
	kernel->resolve_item_traits([](int32_t) { return uint8_t{0}; });
	const int32_t block[6] = {5 << 16, 7 << 16, 2 << 16, 0x12345678, 0x1111, 0x2222};
	const w::EntityHandle h = kernel->spawn_deployable(1400, 2, block);
	CHECK(h.valid() && h.pool() == 1);
	const w::Entity *e = kernel->world.registry.get(h);
	CHECK(e != nullptr);
	if (e == nullptr) return;
	CHECK(e->position.x.q16 == block[0] && e->position.y.q16 == block[1] &&
			e->position.z.q16 == block[2]);
	CHECK(e->heading == 0x12345678 && e->pitch_bam == 0 && e->roll_bam == 0);
	CHECK(e->team == 2 && e->net_id == 12000 && e->ref_num == 0 && e->group_id == 0);
	CHECK((e->flags & 0x1000u) != 0);
	CHECK(e->health == 600);
	CHECK(e->ai_text_file == "Deployable_Boat_");
	CHECK(e->spawn_phase == 0 && kernel->world.vehicle_ai_spawn_phase == 1);
	const w::AiEntity *ai = kernel->world.ai.for_handle(h);
	CHECK(ai != nullptr && ai->net_id == 12000 && ai->team == 2);
	// The next brain takes the next phase.
	const w::EntityHandle second = kernel->spawn_deployable(1400, 1, block);
	CHECK(second.valid() && kernel->world.registry.get(second)->spawn_phase == 1);
	// The first row's type and a type no row carries spawn nothing.
	CHECK(!kernel->spawn_deployable(1, 2, block).valid());
	CHECK(!kernel->spawn_deployable(1999, 2, block).valid());
}

struct BayRig {
	std::unique_ptr<w::World> wp = std::make_unique<w::World>();
	w::World &world = *wp;
	std::unique_ptr<inmatch::NapiNPServerCtx> ctx = std::make_unique<inmatch::NapiNPServerCtx>();
	std::vector<inmatch::NapiNPConnection> conns; // one, off the stack
	w::EntityHandle bay, player, spawned;
	int32_t got[6] = {};
	uint16_t got_item = 0;
	uint8_t got_team = 0;

	BayRig() {
		world.registry.configure_pool(0, 8);
		world.registry.configure_pool(1, 8);
		w::Entity b;
		b.kind = w::EntityKind::Item;
		b.has_item_def = true;
		b.vehicle_spawn_groups = {3};
		b.vehicle_spawn_ids = {101400};
		b.position.set_q16({50 << 16, 60 << 16, 7 << 16});
		b.heading = 0;
		bay = world.registry.spawn(1, b);
		w::Entity p;
		p.kind = w::EntityKind::Organic;
		p.position.set_q16({52 << 16, 60 << 16, 7 << 16});
		p.health = p.health_max = 100;
		p.alive = true;
		player = world.registry.spawn(0, p);
		ctx->is_authority = true;
		conns.push_back(conn_fixture::make_conn(2, 1, nullptr, replication::TransportMode::Client,
				player, true));
		conns[0].assigned_team = 2;
		conns[0].assigned_team_valid = true;
		ctx->deployable_spawner = [this](w::World &wd, uint16_t item_id, uint8_t team,
				const int32_t position[6]) {
			std::copy(position, position + 6, got);
			got_item = item_id;
			got_team = team;
			w::Entity v;
			v.kind = w::EntityKind::Item;
			v.has_item_def = true;
			v.has_graphic_model = true;
			v.graphic_model_userpoints = 4;
			v.item_attrib = w::kItemAttribPlayerControl;
			v.position.set_q16({position[0], position[1], position[2]});
			v.health = v.health_max = 500;
			v.alive = true;
			w::Seat sit;
			sit.type = w::SeatType::Passenger;
			sit.bone_index = 2;
			sit.retail_slot = 0;
			sit.source_name = "sitex00";
			v.seats.push_back(sit);
			w::Seat ctrl;
			ctrl.type = w::SeatType::Controller;
			ctrl.bone_index = 4;
			ctrl.retail_slot = 8;
			ctrl.source_name = "ctrlx00";
			v.seats.push_back(ctrl);
			spawned = wd.registry.spawn(1, v);
			return spawned;
		};
	}
	bool request() {
		VehicleSpawnRequest r;
		r.source_handle = bay.packed;
		r.type_index = 3;
		return inmatch::Server_HandleVehicleSpawnRequest(*ctx, conns[0], r, world);
	}
};

void test_the_bay_spawns_at_its_userpoint_and_mounts_the_control_bone() {
	BayRig r;
	w::Entity &bay = *r.world.registry.get(r.bay);
	bay.deploy_point_valid = true;
	bay.deploy_point[0] = 0x10000;
	bay.deploy_point[1] = 0x20000;
	bay.deploy_point[2] = -0x8000;
	CHECK(r.request());
	CHECK(r.got_item == 1400 && r.got_team == 2);
	// Yaw, pitch and roll 0: the identity rotation, so the point rides the bay exactly.
	CHECK(r.got[0] == (50 << 16) + 0x10000 && r.got[1] == (60 << 16) + 0x20000 &&
			r.got[2] == (7 << 16) - 0x8000);
	CHECK(r.got[3] == 0 && r.got[4] == 0 && r.got[5] == 0);
	const w::Entity *vehicle = r.world.registry.get(r.spawned);
	const w::Entity *player = r.world.registry.get(r.player);
	CHECK(vehicle != nullptr && vehicle->team == 2);
	CHECK(player != nullptr && player->mounted && player->mount_target == r.spawned &&
			player->mount_bone == 4);
	// A turned bay carries its attitude into the block and turns the point.
	BayRig t;
	w::Entity &turned = *t.world.registry.get(t.bay);
	turned.heading = 0x40000000;
	turned.pitch_bam = 0x0100;
	turned.roll_bam = 0x0200;
	turned.deploy_point_valid = true;
	turned.deploy_point[0] = 0x30000;
	CHECK(t.request());
	const int32_t pos[3] = {50 << 16, 60 << 16, 7 << 16};
	int32_t want[3] = {};
	w::collision_matrix_from_euler(0x40000000, 0x0100, 0x0200, pos)
			.transform_point(turned.deploy_point, want);
	CHECK(t.got[0] == want[0] && t.got[1] == want[1] && t.got[2] == want[2]);
	CHECK(t.got[3] == 0x40000000 && t.got[4] == 0x0100 && t.got[5] == 0x0200);
}

void test_a_bay_without_the_points_raises_its_spawn() {
	BayRig r;
	CHECK(r.request());
	CHECK(r.got[0] == (50 << 16) && r.got[1] == (60 << 16) && r.got[2] == (7 << 16) + 0x20000);
	// No spawner bound: the request spawns nothing.
	BayRig n;
	n.ctx->deployable_spawner = nullptr;
	CHECK(!n.request());
}

// The class init on a placed pool-1 row (the deployable's, a joiner's 0x0D / 0x18 row): an
// AIData brain-class row takes its brain from the profile its AI slot's +156 name loads,
// else the vehicle family's def default_aip, else helo1, the helicopter family going
// straight to helo1; a row of any other def takes none; each brain takes the next
// stagger phase. The profiles are the test's own, told apart by their aim_skill.
// [orig: Entity_InitVehicleAIFromDef @0x4686C0 (the +156 name @0x46879E..0x4687A3, the
//  def's default_aip @0x4687BC..0x4687C1, helo1 @0x4687D3, the stagger @0x46891C..0x468945);
//  Entity_InitHelicopterAIFromDef @0x4683C0 (the +156 test @0x4684A4, helo1 @0x4684C9)]
void test_the_class_init_names_its_profile() {
	std::array<def::DefItemDef, 4> rows{};
	rows[0].id = mission::kItemIdOffset + 1;
	rows[0].type = 1;
	for (std::size_t i = 1; i < rows.size(); ++i) {
		rows[i].id = mission::kItemIdOffset + 1400 + static_cast<int>(i) - 1;
		rows[i].type = 1;
		rows[i].attrib = w::kItemAttribAIData;
		std::strcpy(rows[i].ai_function, "cveh");
		std::strcpy(rows[i].default_aip, "Def_Profile");
	}
	std::strcpy(rows[2].ai_function, "CHel");
	rows[3].attrib = 0;
	def::DefItemsFile items{rows.data(), rows.size()};
	auto kernel = std::make_unique<mission::MissionKernel>();
	kernel->set_items_table(&items);
	for (int pool = 0; pool < 3; ++pool) kernel->world.registry.configure_pool(pool, 8);
	kernel->resolve_item_traits([](int32_t) { return uint8_t{0}; });
	const auto profile = [](int32_t aim_skill) {
		aip::Profile p;
		p.aim_skill = aim_skill;
		return p;
	};
	kernel->ai_profiles = {{"wire_profile", profile(1)}, {"def_profile", profile(2)},
			{"helo1", profile(3)}};
	const auto place = [&](uint16_t item_id, const char *slot_name) {
		w::Entity seed;
		seed.kind = w::EntityKind::Item;
		seed.item_id = item_id;
		seed.has_item_def = true;
		seed.heading = 0x12345678;
		seed.team = 2;
		seed.net_id = 77;
		seed.ai_text_file = slot_name;
		return kernel->world.registry.spawn(1, seed);
	};
	const w::EntityHandle named = place(1400, "Wire_Profile");
	CHECK(kernel->init_item_class_brain(named));
	const w::AiEntity *ai = kernel->world.ai.for_handle(named);
	CHECK(ai != nullptr && ai->profile.accuracy == 1 && ai->team == 2 && ai->net_id == 77 &&
			ai->heading == 0x12345678);
	CHECK(kernel->world.registry.get(named)->spawn_phase == 0);
	const w::EntityHandle unnamed = place(1400, "");
	CHECK(kernel->init_item_class_brain(unnamed));
	ai = kernel->world.ai.for_handle(unnamed);
	CHECK(ai != nullptr && ai->profile.accuracy == 2);
	CHECK(kernel->world.registry.get(unnamed)->spawn_phase == 1);
	const w::EntityHandle helicopter = place(1401, "");
	CHECK(kernel->init_item_class_brain(helicopter));
	ai = kernel->world.ai.for_handle(helicopter);
	CHECK(ai != nullptr && ai->profile.accuracy == 3);
	CHECK(kernel->world.vehicle_ai_spawn_phase == 3);
	const w::EntityHandle plain = place(1402, "Wire_Profile");
	CHECK(!kernel->init_item_class_brain(plain));
	CHECK(kernel->world.ai.for_handle(plain) == nullptr);
	CHECK(kernel->world.vehicle_ai_spawn_phase == 3);
}

} // namespace

int main() {
	test_the_spawner_binds_the_template();
	test_the_bay_spawns_at_its_userpoint_and_mounts_the_control_bone();
	test_a_bay_without_the_points_raises_its_spawn();
	test_the_class_init_names_its_profile();
	if (failures == 0) std::printf("PASS deployable_spawn\n");
	return failures == 0 ? 0 : 1;
}
