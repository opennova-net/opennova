#include "world/lfp_feed.h"

#include "world/world.h"

#include <hud/hud_lfp_panel.h>

#include <cmath>
#include <cstdint>

namespace opennova::world {

namespace {

int32_t to_q16(float units) {
    return static_cast<int32_t>(std::lround(static_cast<double>(units) * 65536.0));
}

// sqrt(dx^2 + dy^2) in Q16, saturated like the retail float chain
// [orig: @0x5987aa..0x5987cf — fild/fmul/fsqrt against flt_7C19E0 = 2147418112.0].
int32_t dist2d_q16(const Vec3 &a, const Vec3 &b) {
    const double dx = (static_cast<double>(a.x) - static_cast<double>(b.x)) * 65536.0;
    const double dy = (static_cast<double>(a.y) - static_cast<double>(b.y)) * 65536.0;
    double d = std::sqrt(dx * dx + dy * dy);
    if (d > 2147418112.0) d = 2147418112.0;
    return static_cast<int32_t>(d);
}

} // namespace

void build_lfp_zones(const World &world, const SpawnZoneRegistry &registry,
                     const Entity &local, int local_team,
                     const LfpZoneTimerLookup &timer,
                     const LfpCaptureFlagsLookup &capture_flags,
                     std::vector<hud::HudLfpZone> &out) {
    out.clear();
    out.reserve(registry.entries.size());
    for (size_t i = 0; i < registry.entries.size(); ++i) {
        const Entity *zone = world.registry.get(registry.entries[i]);
        if (zone == nullptr) continue;
        hud::HudLfpZone row;
        row.letter_index = static_cast<int>(i);
        // The zone's team byte, masked like the panel's count pass
        // [orig: entity+538 & 0x1F @0x5a24d9; the marker's +0x162 read
        //  @0x59873a is the same byte].
        row.team = zone->team & 0x1F;
        LfpZoneTimer t;
        if (timer && timer(zone->handle, t)) {
            row.timer_present = true;
            row.timer_team = t.team;
            row.control = t.control;
            row.rate = t.rate;
            row.value = t.value;
            row.limit = t.limit;
            row.active = t.active;
            row.count_owner = t.count_owner;
            row.count_other = t.count_other;
        }
        row.capture_flags = capture_flags ? capture_flags(zone->handle) : 0;
        // The viewer's 2D distance and the cylinder test
        // [orig: @0x59879f..0x598810 — Position.X/Y deltas, fsqrt, the
        //  +0x15E radius << 16, |dz| against its half].
        const int32_t d2 = dist2d_q16(local.position, zone->position);
        const int32_t dz = to_q16(local.position.z - zone->position.z);
        row.in_cylinder = hud::lfp_in_cylinder(d2, dz, zone->zone_radius);
        // The distance label subtracts the zone entity's own bound radius —
        // `*(*EntryById)`: entry[0] is the zone entity, +0 its bound radius
        // (the same field the camera's eye drop reads @0x43862f) — so the
        // label reads to the objective's EDGE, floored at zero, then >> 16
        // [orig: @0x5990ac..0x599108; the subtrahend load @0x5990f3].
        int32_t edge = d2 - to_q16(zone->bound_radius);
        if (edge < 0) edge = 0;
        row.distance_m = edge >> 16;
        out.push_back(row);
    }
    (void)local_team;
}

} // namespace opennova::world
