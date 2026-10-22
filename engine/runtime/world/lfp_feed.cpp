#include <runtime/world/lfp_feed.h>

#include <runtime/world/world.h>

#include <runtime/hud/hud_lfp_panel.h>

#include <cmath>
#include <cstdint>

#include <base/io/bam.h>

namespace opennova::world {

namespace {

// sqrt(dx^2 + dy^2) in Q16 over the Position words' 32-bit deltas, saturated
// like the retail float chain (D-NET-507) [orig: HUD_DrawZoneMarker @0x5986F0 (the
// X / Y word deltas @0x59879F / @0x59879C, fild/fmul/fsqrt @0x5987AA..0x5987BA, the min
// against flt_7C19E0 = 2147418112.0 @0x5987BC..0x5987CF, _ftol2_sse @0x5987F7)].
int32_t dist2d_q16(const EntityPosition &a, const EntityPosition &b) {
    const double dx = static_cast<double>(io::bam_sub(a.x.q16, b.x.q16));
    const double dy = static_cast<double>(io::bam_sub(a.y.q16, b.y.q16));
    double d = std::sqrt(dx * dx + dy * dy);
    if (d > 2147418112.0) d = 2147418112.0;
    return static_cast<int32_t>(d);
}

} // namespace

void build_lfp_zones(const World &world, const SpawnZoneRegistry &registry,
                     const Entity &local, int local_team,
                     const LfpZoneTimerLookup &timer,
                     const LfpCaptureFlagsLookup &capture_flags,
                     const LfpZoneTeamLookup &team_byte,
                     std::vector<hud::HudLfpZone> &out) {
    out.clear();
    out.reserve(registry.entries.size());
    for (size_t i = 0; i < registry.entries.size(); ++i) {
        const Entity *zone = world.registry.get(registry.entries[i]);
        if (zone == nullptr) continue;
        hud::HudLfpZone row;
        row.letter_index = static_cast<int>(i);
        // The zone byte the panel groups by (+0x21A), and the Team byte the
        // marker reads, sign-extended (+0x162) -- two fields
        // [orig: HUD_DrawZoneStatusPanel `mov al, [eax+21Ah]` @0x5a24cd;
        //  HUD_DrawZoneMarker `mov al, [ebp+162h]; movsx eax, al` @0x598734..0x59873a].
        row.zone_byte = zone->zone_number;
        row.team = team_byte ? team_byte(*zone) : static_cast<int8_t>(zone->team);
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
        // The viewer's 2D distance and the cylinder test: the Z word delta's
        // magnitude against half the +0x15E radius << 16
        // [orig: HUD_DrawZoneMarker @0x5986F0 (the delta @0x5987D4, cdq / xor /
        //  sub, the radius @0x5987D7 / @0x5987F4, the half @0x598805..0x59880A)].
        const int32_t d2 = dist2d_q16(local.position, zone->position);
        const int32_t dz = io::bam_sub(local.position.z.q16, zone->position.z.q16);
        row.in_cylinder = hud::lfp_in_cylinder(d2, dz, zone->zone_radius);
        // The distance label subtracts the zone entity's own bound radius —
        // `*(*EntryById)`: entry[0] is the zone entity, +0 its bound radius
        // (the same field the camera's eye drop reads @0x43862f) — so the
        // label reads to the objective's EDGE, floored at zero, then >> 16
        // [orig: @0x5990ac..0x599108; the subtrahend load @0x5990f3].
        int32_t edge = d2 - zone->bound_radius.q16;
        if (edge < 0) edge = 0;
        row.distance_m = edge >> 16;
        out.push_back(row);
    }
    (void)local_team;
}

} // namespace opennova::world
