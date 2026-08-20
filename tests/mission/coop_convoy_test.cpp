// Asset-gated end-to-end convoy test: the 05TRcoop Stryker departs when the
// player takes its gun seat — the REAL mission data driving the whole chain:
//
//   the player seated on the Stryker's emplacement child
//     -> BMS event 81: Single/sub42(p1=10000 player sentinel, p2=21) through
//        the seated carrier fold [orig: Entity_IsOnTopOfChain @0x4f19a0]
//        AND group 10 inside area 3 (the Stryker's own parking spot)
//     -> BMS event 82 (2 s activation delay): MisvarChange var20=1 +
//        RedirectGroupTo group 10 -> route list 7
//     -> the vehicle brain takes route 7 and the SM mover drives the hull.
//
// This is the scripted behavior a live host shows when a player mounts the
// Stryker gun; the test pins it deterministically against the shipped mission.
// Gated on OPENNOVA_JO_DIR (skip-as-pass without a JO install).
#include "mission/event_runtime.h"
#include "mission/promote.h"

#include "mission/bms.h"

#include "world/ai.h"
#include "world/player_spawn.h"
#include "world/vehicle_mount.h"
#include "world/world.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace opennova;
namespace w = opennova::world;

int failures = 0;
bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	++failures;
	return false;
}

} // namespace

int main() {
	const char *dir = std::getenv("OPENNOVA_JO_DIR");
	if (dir == nullptr || *dir == '\0') {
		std::printf("coop convoy: SKIP (OPENNOVA_JO_DIR not set)\n");
		return 0;
	}
	const std::string path = std::string(dir) + "/05TRcoop.bms";
	std::ifstream f(path, std::ios::binary);
	if (!f) {
		std::printf("coop convoy: SKIP (no 05TRcoop.bms under OPENNOVA_JO_DIR)\n");
		return 0;
	}
	std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
	                           std::istreambuf_iterator<char>());
	bms::File m;
	std::string error;
	if (!expect(bms::parse(bytes.data(), bytes.size(), m, error),
	            "05TRcoop.bms parses")) {
		std::fprintf(stderr, "  parse error: %s\n", error.c_str());
		return 1;
	}

	w::World world;
	w::AiSystem ai;
	world.ai = &ai;
	mission::BmsEventSystem events;
	events.load(m.events, m.triggers, m.actions);

	// The Stryker (type 2015) needs a control seat for promote to grant it a
	// vehicle brain; supply the minimal spec (the live game derives it from the
	// item database).
	mission::PromoteOptions opts;
	mission::ItemSeatSpec stryker_seats;
	stryker_seats.type_id = 2015;
	w::Seat driver;
	driver.type = w::SeatType::Driver;
	stryker_seats.seats.push_back(driver);
	opts.item_seat_specs.push_back(stryker_seats);
	// The Blackhawk (2010) needs its control seat AT promote so the vehicle
	// brain exists for the flight leg.
	mission::ItemSeatSpec helo_seats;
	helo_seats.type_id = 2010;
	w::Seat helo_ctrl;
	helo_ctrl.type = w::SeatType::Controller;
	helo_seats.seats.push_back(helo_ctrl);
	opts.item_seat_specs.push_back(helo_seats);
	const mission::PromoteResult promo = mission::promote_mission(m, world, ai, opts);
	expect(promo.nav_channels > 0, "nav channels promoted");

	world.add_system(&events);
	world.add_system(&ai);
	world.load_systems();

	const w::EntityHandle stryker = world.registry.find_by_net_id(21);
	w::Entity *veh = world.registry.get(stryker);
	if (!expect(veh != nullptr, "Stryker SSN 21 promoted")) return 1;
	const float start_x = veh->position.x;
	const float start_y = veh->position.y;

	// The player, seated on the Stryker's emplaced-cannon child — the exact
	// live scenario (the gunner's carrier chain: player -> gun -> hull).
	w::PlayerSpawn ps;
	ps.position = {veh->position.x + 2.0f, veh->position.y, veh->position.z};
	ps.team = 1;
	const w::EntityHandle player = w::spawn_player(world, ps);
	if (!expect(player.valid(), "player spawned")) return 1;
	world.cached.local_player = player;
	w::Entity gun_seed{};
	gun_seed.alive = true;
	gun_seed.net_id = 60001;
	gun_seed.item_id = 2016;
	gun_seed.position = veh->position;
	w::Seat gun_seat;
	gun_seat.type = w::SeatType::Gunner;
	gun_seed.seats.push_back(gun_seat);
	const w::EntityHandle gun = world.registry.spawn(1, gun_seed);
	world.registry.get(gun)->emplacement_parent = stryker;
	// The orphan peeler validates the parent's spawn generation every tick; an
	// unstamped id reads as a dead parent and dismounts the gunner.
	world.registry.get(gun)->emplacement_parent_spawn_id = veh->registry_spawn_id;
	w::Entity *pl = world.registry.get(player);
	pl->item_id = 5305; // ItemTypeIndex gate [orig: @0x4f19a0]
	if (!expect(world.commands.mount(10000, 60001),
	            "the player mounts the gun through the real seat machinery"))
		return 1;

	expect(world.commands.ssn_on_chain_of(10000, 21),
	       "the seated player is on the Stryker's carrier chain");

	// Run the mission: the quarter pass evaluates every 16 ticks, event 82
	// carries a 2 s activation delay, then the route order lands and the SM
	// mover drives. 2500 ticks = ~40 s of mission time.
	for (int t = 0; t < 2500; ++t) world.run_logic_tick(/*is_authority=*/true);

	std::printf("diag: fired80=%d fired81=%d fired82=%d postMounted=%d postChain=%d\n",
	            int(events.event_fired(80)), int(events.event_fired(81)),
	            int(events.event_fired(82)),
	            int(world.registry.get(player)->mounted),
	            int(world.commands.ssn_on_chain_of(10000, 21)));
	w::AiEntity *brain = ai.for_handle(stryker);
	expect(brain != nullptr, "the Stryker carries a vehicle brain");
	if (brain != nullptr) {
		std::printf("diag: wpType=%d wpChannel=%d wpNode=%d aiPos=(%.1f, %.1f)\n",
		            brain->brain.f[w::AiBrain::kWpType],
		            brain->brain.f[w::AiBrain::kWpChannel],
		            brain->brain.f[w::AiBrain::kWpNode],
		            brain->pos[0] / 65536.0, brain->pos[1] / 65536.0);
		expect(brain->brain.f[w::AiBrain::kWpChannel] == 7,
		       "event 82 routed the Stryker onto list 7");
	}
	// Displacement asserts on the AI domain: without item vehicle traits (the
	// live game supplies them from the item database) the SM kinematic mover
	// drives the brain entity and the registry mirror in the traits-gated motor
	// pass never runs — the ROUTE traversal is the behavior under test.
	if (brain != nullptr) {
		const float dx = brain->pos[0] / 65536.0f - start_x;
		const float dy = brain->pos[1] / 65536.0f - start_y;
		expect(dx * dx + dy * dy > 25.0f * 25.0f,
		       "the Stryker drove its route (>25 u from the parking spot)");
		expect(brain->brain.f[w::AiBrain::kWpNode] >= 20,
		       "the route was substantially traversed");
	}

	// ---- The helo AI flight: an AI pilot in the Blackhawk's control seat
	// flies route 6 (the mission's authored helo route, RedirectGroupTo group 9
	// -> list 6) with altitude gain. Exercises chel_ai_drive + the shared
	// aircraft mover's authority leg end to end on the real mission data.
	// [orig: the CHel AI leg of Entity_UpdateAircraftPhysics @0x490310]
	{
		const w::EntityHandle helo = world.registry.find_by_net_id(420);
		w::Entity *hv = world.registry.get(helo);
		if (expect(hv != nullptr, "helo SSN 420 promoted")) {
			// The item db is absent in a bare engine world: supply the traits
			// row (family Helicopter) and a control seat, as the live game
			// derives from items.def.
			w::VehicleTraits ht;
			ht.family = w::VehicleFamily::Helicopter;
			ht.player_control = true;
			ht.turn_rate = 8000000;
			ht.acceleration = 1200;
			// The air vertical clamp [+cs, -2cs] — zero pins the climb servo
			// to the ground; the live game reads itemDef+0x920.
			ht.climb_speed = 30000;
			world.vehicle_traits.set(hv->item_id, ht);
			if (hv->seats.empty()) {
				w::Seat ctrl;
				ctrl.type = w::SeatType::Controller;
				hv->seats.push_back(ctrl);
			}
			// An AI pilot seated in control.
			w::Entity pilot_seed{};
			pilot_seed.alive = true;
			pilot_seed.net_id = 61001;
			pilot_seed.item_id = 2063;
			pilot_seed.kind = w::EntityKind::Organic;
			pilot_seed.position = hv->position;
			world.registry.spawn(0, pilot_seed);
			expect(world.commands.mount(61001, 420), "AI pilot mounts the helo");
			// The mission's authored helo route order.
			expect(world.commands.set_ssn_waypoint(420, 6, 0),
			       "route 6 lands on the helo");

			const float hx = hv->position.x, hy = hv->position.y,
			            hz = hv->position.z;
			for (int t = 0; t < 3000; ++t)
				world.run_logic_tick(/*is_authority=*/true);

			const w::Entity *ha = world.registry.get(helo);
			const float dx = ha->position.x - hx;
			const float dy = ha->position.y - hy;
			std::printf("diag helo: moved=(%.1f, %.1f) climb=%.1f\n",
			            dx, dy, ha->position.z - hz);
			{
				const w::AiEntity *hb = ai.for_handle(helo);
				std::printf("diag helo2: brain=%d wpType=%d ch=%d cmd=%d alt=%d "
				            "engine=%d yawSeeded=%d\n",
				            int(hb != nullptr),
				            hb ? hb->brain.f[w::AiBrain::kWpType] : -1,
				            hb ? hb->brain.f[w::AiBrain::kWpChannel] : -1,
				            ha->veh.cmd_speed, ha->veh.net_alt_target,
				            int(ha->veh.net_engine_on), int(ha->veh.yaw_seeded));
			}
			expect(dx * dx + dy * dy > 25.0f * 25.0f,
			       "the helo flew its route (>25 u horizontal)");
			expect(ha->position.z - hz > 10.0f,
			       "the helo climbed (>10 u altitude gain)");
		}
	}

	if (failures == 0) std::printf("coop convoy: the Stryker departs on the player's mount\n");
	return failures ? 1 : 0;
}
