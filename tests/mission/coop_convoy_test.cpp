// Asset-gated end-to-end convoy test: the 05TRcoop Stryker departs when the
// player takes its gun seat — the REAL mission data driving the whole chain:
//
//   the player seated on the Stryker's emplacement child
//     -> BMS event 81: Single/sub42(p1=10000 player sentinel, p2=21) through
//        the seated carrier fold [orig: Entity_IsOnTopOfChain @0x4f19a0]
//        AND group 10 inside area 3 (the Stryker's own parking spot)
//     -> BMS event 82 (2 s activation delay): MisvarChange var20=1 +
//        RedirectGroupTo group 10 -> route list 7
//     -> the vehicle brain takes route 7 and the motor drives the hull.
//
// This is the scripted behavior a live host shows when a player mounts the
// Stryker gun; the test pins it deterministically against the shipped mission
// booted through the engine's own mission kernel (ADR 0042 d3): the Stryker's
// and the Blackhawk's control seats come from their models, the vehicle traits
// from items.def, the ground from the mission terrain, the hulls from the
// collision instances - the live host's world, not a hand-typed seat table.
// Gated on OPENNOVA_JO_DIR (reports Skipped without a JO install).
#include <runtime/mission/event_runtime.h>

#include <formats/mission/bms.h>

#include <runtime/world/ai.h>
#include <runtime/world/vehicle_motor.h>
#include <runtime/world/vehicle_mount.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "common/retail_mission_files.h"
#include "common/retail_mission_open.h"
#include "common/retail_paths.h"

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
	RETAIL_REQUIRE_OR_SKIP(install, retail::install(),
			"OPENNOVA_JO_DIR (a retail JO install serving 05TRcoop.bms)");
	testrig::RetailMissionRig rig;
	std::string error, served_by;
	if (!retail::open_mission(rig, install, "05TRcoop.bms", error, served_by))
		return retail::skip("05TRcoop.bms on the OPENNOVA_JO_DIR mount (base or an expansion) "
		                    "or loose under OPENNOVA_JO_ASSETS");
	// The bare no-net tick: the scripted chain under test needs no session,
	// and the host's own player spawns at the mission start marker.
	testrig::BootOptions options;
	options.listen_server = false;
	if (!expect(rig.boot(options, error), "05TRcoop boots through the mission kernel")) {
		std::fprintf(stderr, "  %s\n", error.c_str());
		return 1;
	}
	w::World &world = rig.world;
	w::AiSystem &ai = rig.ai;
	mission::BmsEventSystem &events = rig.events;
	expect(rig.promo.nav_channels > 0, "nav channels promoted");
	if (!expect(rig.has_local_player(), "the host's own player spawned")) return 1;
	std::printf("kernel boot: %zu seat specs, terrain %s, %d collision instances, wac %s, "
	            "mission served by %s\n",
			rig.seat_specs.size(), rig.has_terrain() ? "loaded" : "NOT LOADED",
			rig.collision_attached, rig.wac_loaded ? "loaded" : "absent", served_by.c_str());

	const w::EntityHandle stryker = world.registry.find_by_net_id(21);
	w::Entity *veh = world.registry.get(stryker);
	if (!expect(veh != nullptr, "Stryker SSN 21 promoted")) return 1;
	// The Stryker (type 2015) needs a control seat for promote to grant it a
	// vehicle brain; the kernel's seat-spec extraction reads it off the model's
	// seat userpoints [orig: seat typing Entity_GetBoneSlotType @0x434ed0].
	expect(!veh->seats.empty(), "the Stryker's seats come from its model");
	const float start_x = veh->position.x;
	const float start_y = veh->position.y;

	// The player, seated on the Stryker's emplaced-cannon child — the exact
	// live scenario (the gunner's carrier chain: player -> gun -> hull). The
	// kernel spawned the player at the start marker; walk it to the hull and
	// seat it on a synthetic emplacement child parented to the Stryker.
	rig.teleport_local_player(
			w::Vec3{veh->position.x + 2.0f, veh->position.y, veh->position.z}, 0.0, 0.0);
	const w::EntityHandle player = world.cached.local_player;
	w::Entity gun_seed{};
	gun_seed.alive = true;
	gun_seed.net_id = 60001;
	gun_seed.item_id = 2016;
	gun_seed.position = veh->position;
	w::Seat gun_seat;
	gun_seat.type = w::SeatType::Gunner;
	gun_seed.seats.push_back(gun_seat);
	const w::EntityHandle gun = world.registry.spawn(1, gun_seed);
	if (!expect(gun.valid(), "a pool-1 slot for the synthetic gun child")) return 1;
	world.registry.get(gun)->emplacement_parent = stryker;
	// The orphan peeler validates the parent's spawn generation every tick; an
	// unstamped id reads as a dead parent and dismounts the gunner.
	world.registry.get(gun)->emplacement_parent_spawn_id = veh->registry_spawn_id;
	if (!expect(world.commands.mount(10000, 60001),
	            "the player mounts the gun through the real seat machinery"))
		return 1;

	expect(world.commands.ssn_on_chain_of(10000, 21),
	       "the seated player is on the Stryker's carrier chain");

	// Run the mission: the quarter pass evaluates every 16 ticks, event 82
	// carries a 2 s activation delay, then the route order lands and the
	// motor drives. 2500 ticks = ~40 s of mission time.
	rig.tick(2500);

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
	// Displacement asserts on the AI domain: with the items.def traits the
	// motor pass drives the hull and mirrors it back into the brain, so the
	// ROUTE traversal is the behavior under test on the real ground.
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
			// The kernel resolves the traits row (family Helicopter, the air
			// vertical clamp from itemDef+0x920) from items.def and the control
			// seat from the model; a mount whose items.def lacks the row keeps
			// the pre-kernel stand-in so the flight leg still runs, and says so.
			if (world.vehicle_traits.get(hv->item_id) == nullptr) {
				std::printf("diag helo: no items.def traits row for item %d - stand-in row\n",
				            hv->item_id);
				w::VehicleTraits ht;
				ht.family = w::VehicleFamily::Helicopter;
				ht.player_control = true;
				ht.turn_rate = 8000000;
				ht.acceleration = 1200;
				ht.climb_speed = 30000;
				world.vehicle_traits.set(hv->item_id, ht);
			}
			expect(!hv->seats.empty(), "the Blackhawk's control seat comes from its model");
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
			expect(world.registry.spawn(0, pilot_seed).valid(), "a pool-0 slot for the pilot");
			expect(world.commands.mount(61001, 420), "AI pilot mounts the helo");
			// The AI PROFILE TYPE selects the rotor machine — type 1 picks the
			// HELO twin, and an unresolved 0 runs NEITHER machine [orig: the
			// class gate `brain+4 -> profile+16 == 2` @0x4928C9..0x4928D1 / the
			// `== 1` twin @0x48FA70]. The kernel's .aip install resolves it from
			// the mission's profile set; an unresolved row keeps the stand-in.
			if (w::AiEntity *hb = ai.for_handle(helo)) {
				if (hb->profile.type == 0) {
					std::printf("diag helo: profile type unresolved - stand-in type 1\n");
					hb->profile.type = 1;
				}
			}
			// The mission's authored helo route order.
			expect(world.commands.set_ssn_waypoint(420, 6, 0),
			       "route 6 lands on the helo");

			const float hx = hv->position.x, hy = hv->position.y,
			            hz = hv->position.z;
			rig.tick(3000);

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

			// The BLADES. 3000 ticks is well past the 1153-tick spool-up, so a
			// crewed helo must be at the cap, its phase must be advancing, and
			// the angle both rotor registers read must stay inside the 16 bits
			// the model takes. A static rotor fails the first of these.
			// [orig: Entity_UpdatePartSpinAccumulator @0x4928B0]
			expect(ha->veh.part_spin.speed == 214748352,
			       "the rotor reached full speed");
			const int32_t phase_a = ha->veh.part_spin.angle;
			const int32_t reg_a = w::vehicle_ctrl_registers(ha->veh).rotor;
			rig.tick();
			const w::Entity *hb2 = world.registry.get(helo);
			expect(hb2->veh.part_spin.angle != phase_a, "the blade phase advances");
			expect(w::vehicle_ctrl_registers(hb2->veh).rotor != reg_a,
			       "the rotor register the model reads changes tick to tick");
			const int32_t reg_b = w::vehicle_ctrl_registers(hb2->veh).rotor;
			expect(reg_a >= 0 && reg_a <= 0xFFFF && reg_b >= 0 && reg_b <= 0xFFFF,
			       "the rotor register stays a u16");
			std::printf("diag rotor: speed=%d phase=%d reg %d -> %d\n",
			            hb2->veh.part_spin.speed, hb2->veh.part_spin.angle, reg_a, reg_b);
		}
	}

	// ---- Pinned-garrison node dump (diagnostic, not asserted): the three
	// soldiers that still fail to walk on a live host all stand INSIDE the east
	// bunker (item 1359 at 159.9, 319.7). Print each one's authored route node
	// so the geometry question — is the node behind an interior wall? — can be
	// answered from the shipped mission instead of guessed.
	{
		for (const int32_t ssn : {233, 1254, 2393}) {
			const w::EntityHandle h = world.registry.find_by_net_id(ssn);
			const w::AiEntity *sb = h.valid() ? ai.for_handle(h) : nullptr;
			if (sb == nullptr) {
				std::printf("diag pinned %d: no brain\n", ssn);
				continue;
			}
			const int32_t ch = sb->slot.f[37];
			const int32_t node = sb->slot.f[38];
			std::printf("diag pinned %d: pos=(%.1f, %.1f, %.1f) slotCh=%d slotNode=%d\n",
			            ssn, sb->pos[0] / 65536.0, sb->pos[1] / 65536.0,
			            sb->pos[2] / 65536.0, ch, node);
			const w::NavChannel *nc = ai.nav.channel(ch);
			if (nc == nullptr) continue;
			for (int32_t i = 0; i < nc->count && i < 32; ++i) {
				const w::NavEntry *ne = ai.nav.entry(nc->entries[i]);
				if (ne == nullptr) continue;
				const double nx = ne->f[1] / 65536.0, ny = ne->f[2] / 65536.0;
				// Only nodes near the bunker matter for the wall question.
				const double bdx = nx - 159.9, bdy = ny - 319.7;
				if (bdx * bdx + bdy * bdy > 20.0 * 20.0) continue;
				std::printf("    ch%d node%d = (%.1f, %.1f, %.1f) radius=%.2f%s\n",
				            ch, i, nx, ny, ne->f[3] / 65536.0, ne->f[0] / 65536.0,
				            i == node ? "  <== current target" : "");
			}
		}
	}

	if (failures == 0) std::printf("coop convoy: the Stryker departs on the player's mount\n");
	return failures ? 1 : 0;
}
