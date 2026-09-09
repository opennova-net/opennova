// Infantry movement-goal detour. [orig: ai_find_cover_position @ 0x4AFAB0]
// The historic name says "cover"; both rays actually seek a clear path TO the goal.
#include <runtime/world/infantry_internal.h>
#include <runtime/world/collision.h>
#include <runtime/world/dir_table.h>
#include <runtime/world/world.h>
#include <base/io/bam.h>

#include <algorithm>
#include <cmath>

namespace opennova::world {

void infantry_detour(AiSystem &ai, AiEntity &e, World &world) {
    auto &inf = e.inf;
    int32_t goal[3] = {inf.move_target[0], inf.move_target[1],
                      io::bam_add(inf.move_target[2], 8192)};
    int32_t next[3] = {goal[0], goal[1], goal[2]};

    // Retail creates a fresh CAIPath with capacity zero (40942A), passes that
    // capacity to FindPath (409677), and cannot emit a node (408E47). Its
    // GetCurrentNode therefore leaves this endpoint alone, even with an AIN.
    if (inf.path_state == 2) {
        std::copy_n(inf.detour_target, 3, next);
    } else if (inf.path_state == 1 || inf.path_state == 3) {
        CollisionWorld terrain_query;
        terrain_query.terrain = world.tables.terrain ? world.tables.terrain : ai.terrain;
        CollisionWorld &query = ai.collision ? *ai.collision : terrain_query;
        EntityHandle ignore = e.handle;
        if (e.slot.f[36] > 0) {
            const EntityHandle cached{static_cast<uint16_t>(e.slot.f[36] - 1)};
            if (world.registry.get(cached)) ignore = cached;
        }
        // Retail initializes this height for changed modes or state 1. No state-3
        // producer is witnessed; its same-mode arm reads an uninitialized stack
        // word. Keep the ground probe deterministic for that malformed state.
        const int32_t start[3] = {e.pos[0], e.pos[1], io::bam_add(
                query.raycast_ground(world, e.handle, e.pos, 0, 0, 65536, 131072, nullptr),
                24576)};
        const int32_t base = io::bam_sub(
                bearing_to(io::bam_sub(next[0], start[0]), io::bam_sub(next[1], start[1])),
                1431655680);
        int best = 50000;
        for (int radius = 2, distance_cost = 10; radius <= 15; ++radius, distance_cost += 5) {
            int32_t angle = base;
            for (int degrees = -120; degrees <= 120;
                    degrees += 30, angle = io::bam_add(angle, 357913920)) {
                int score = distance_cost + std::abs(degrees);
                if (score >= best ||
                        io::bam_abs(io::bam_sub(angle, inf.body_heading)) < 357913920)
                    continue;
                int32_t c, s;
                quantized_dir(angle, c, s);
                const int32_t dx = static_cast<int32_t>((int64_t(c) * (radius << 16)) >> 22);
                const int32_t dy = static_cast<int32_t>((int64_t(s) * (radius << 16)) >> 22);
                const int32_t candidate[3] = {
                    io::bam_add(start[0], dx), io::bam_add(start[1], dy),
                    io::bam_add(query.raycast_ground(world, e.handle, e.pos, dx, dy,
                                                     65536, 131072, nullptr), 28672)};
                if (io::bam_abs(io::bam_sub(candidate[2], start[2])) > (radius << 16) ||
                        !query.entity_los_clear(world, e.handle, ignore, start, candidate, 8192))
                    continue;
                if (candidate[2] < world.env.water_z) score += 195;
                if (score >= best) continue;
                if (inf.path_state == 1 && score + 390 <= best) {
                    best = score + 390;
                    std::copy_n(candidate, 3, next);
                }
                if (query.entity_los_clear(world, e.handle, ignore, candidate, goal, 8192)) {
                    best = score;
                    std::copy_n(candidate, 3, next);
                }
            }
        }
        // Even an unsuccessful search caches the unchanged goal and enters 2.
        // [orig: @0x4AFEA8]
        std::copy_n(next, 3, inf.detour_target);
        inf.path_state = 2;
    }

    const int32_t dx = io::bam_sub(next[0], e.pos[0]);
    const int32_t dy = io::bam_sub(next[1], e.pos[1]);
    const int32_t distance = static_cast<int32_t>(
            std::min(std::hypot(double(dx), double(dy)), 2147418112.0));
    if (distance < 65536) inf.path_state = 0; // [orig: @0x4AFF06]
    inf.target_heading = bearing_to(dx, dy);
    // The retail helper always returns 1; its caller's failure-mode ladder is dead.
}

} // namespace opennova::world
