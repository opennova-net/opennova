// Walk onto the 09TR Chinook floor without Use. The mission itself orders six
// soldiers into seats, leaves the instructor standing, and starts the flight.
#include "common/retail_mission_files.h"
#include "common/retail_paths.h"
#include <runtime/world/vehicle_mount.h>
#include <cmath>
#include <cstdio>
#include <memory>

using namespace opennova;
namespace {
world::Vec3 local_offset(const world::Entity &carrier, world::Vec3 position) {
    const auto sub = [](auto a, auto b) { return world::Vec3{a.x-b.x, a.y-b.y, a.z-b.z}; };
    const auto dot = [](auto a, auto b) { return a.x*b.x + a.y*b.y + a.z*b.z; };
    const auto delta = sub(position, carrier.position);
    return {
        dot(delta, sub(world::entity_local_point_world(carrier, {1,0,0}), carrier.position)),
        dot(delta, sub(world::entity_local_point_world(carrier, {0,1,0}), carrier.position)),
        dot(delta, sub(world::entity_local_point_world(carrier, {0,0,1}), carrier.position))};
}
}
int main() {
    RETAIL_REQUIRE_OR_SKIP(root, retail::assets(), "OPENNOVA_JO_ASSETS with 09TR.bms");
    auto owned = std::make_unique<testrig::RetailMissionRig>();
    auto &r = *owned;
    std::string error;
    if (!r.open(root, "09TR.bms", error)) return retail::skip(error.c_str());
    if (!r.boot(testrig::BootOptions{}, error)) {
        std::printf("FAIL 09TR boot: %s\n", error.c_str());
        return 1;
    }
    r.tick(62);
    auto *heli = r.world.registry.by_net_id(4572);
    if (!heli) { std::puts("FAIL missing training helicopter"); return 1; }
    const auto carrier = heli->handle;
    const auto player = r.world.cached.local_player;
    const auto rest = heli->position;
    const auto rest_yaw = heli->yaw;
    // Stage above the floor; production collision must acquire support. No
    // ground link, mount, route, event, or vehicle pose is injected by the test.
    r.world.commands.set_entity_position(player,
            world::entity_local_point_world(*heli, {0,5,-4.5f}));
    bool supported = false, airborne = false;
    world::Vec3 standing{};
    for (int tick = 0; tick < 3125; ++tick) { // 50 seconds, including climb/turn
        r.tick();
        heli = r.world.registry.get(carrier);
        const auto *p = r.world.registry.get(player);
        if (!heli || !p || !p->alive || p->mounted) {
            std::puts("FAIL free-standing player lost"); return 1;
        }
        const auto local = local_offset(*heli, p->position);
        if (!supported && p->ground_target == carrier) {
            supported = true;
            standing = local;
        }
        if (heli->position.z - rest.z > 4) airborne = true;
        if (supported && (std::abs(local.x - standing.x) > 1.0f ||
                std::abs(local.y - standing.y) > 1.0f || local.z < -6.8f || local.z > -3.5f)) {
            std::printf("FAIL player left cabin at tick %d: local=(%.3f,%.3f,%.3f)\n",
                        tick, local.x, local.y, local.z);
            return 1;
        }
        if (!airborne) continue;
        for (int ssn : {4661,4657,4658,4659,6036,4660,4664}) {
            const auto *npc = r.world.registry.by_net_id(ssn);
            const auto offset = npc ? local_offset(*heli, npc->position) : world::Vec3{};
            if (!npc || !npc->alive || offset.z < -6.8f || offset.z > -3.5f ||
                    std::abs(offset.x) > 3 || offset.y < 2 || offset.y > 8 ||
                    (ssn != 4664 && (!npc->mounted || npc->mount_target != carrier)) ||
                    (ssn == 4664 && npc->mounted)) {
                std::printf("FAIL passenger %d left cabin at tick %d: local=(%.3f,%.3f,%.3f)\n",
                            ssn, tick, offset.x, offset.y, offset.z);
                return 1;
            }
        }
    }
    const float dx = heli->position.x - rest.x, dy = heli->position.y - rest.y;
    if (!supported || !airborne || dx*dx + dy*dy < 100 || heli->yaw == rest_yaw ||
            !r.events.event_fired(25) || !r.events.event_fired(26) || !r.events.event_fired(27)) {
        std::printf("FAIL takeoff did not exercise translation, turn, and boarding orders\n");
        return 1;
    }
    std::printf("09TR: player and instructor stayed on floor, six NPCs boarded; flight delta=(%.2f,%.2f,%.2f)\n",
                dx, dy, heli->position.z - rest.z);
    return 0;
}
