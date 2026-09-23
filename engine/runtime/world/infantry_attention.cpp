// The org1 think's post-commit tail: the ride link, the idle facing fan, the
// stop fix-up, NPC attention, independent look, and the idle spotting side
// effects. [orig: Entity_UpdateInfantryAI @0x4B9910, ride link
//  @0x4BD87E..0x4BD905, idle facing @0x4BD905..0x4BE07A, scan
//  @0x4BE0CA..0x4BE463, head tracking @0x4BE463..0x4BE7FD, look chase
//  @0x4BEB18..0x4BEFF0]
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

#include <base/io/bam.h>
#include <formats/def/def.h>
#include <runtime/world/angle.h>
#include <runtime/world/infantry_internal.h>
#include <runtime/world/world.h>

namespace opennova::world {
namespace {

// Position and CameraOffset are both live Q16 lanes in the retail entity.
// Preserve the AI position's low bits instead of round-tripping through floats.
void position_of(const AiSystem &ai, const Entity &entity, int32_t out[3]) {
    if (const AiEntity *body = ai.for_handle(entity.handle)) {
        std::copy_n(body->pos, 3, out);
    } else {
        out[0] = static_cast<int32_t>(entity.position.x * 65536.0f);
        out[1] = static_cast<int32_t>(entity.position.y * 65536.0f);
        out[2] = static_cast<int32_t>(entity.position.z * 65536.0f);
    }
}

void eye_of(const AiSystem &ai, const Entity &entity, int32_t out[3]) {
    position_of(ai, entity, out);
    out[0] = io::bam_add(out[0], entity.eye_offset_x);
    out[1] = io::bam_add(out[1], entity.eye_offset_y);
    out[2] = io::bam_add(out[2], entity.eye_offset_z);
}

int32_t distance_of(int32_t x, int32_t y, int32_t z) {
    // flt_7C19E0 is an UPPER clamp, 2147418112.0f (0x4EFFFE00).
    return static_cast<int32_t>(std::min(2147418112.0,
            std::sqrt(double(x) * x + double(y) * y + double(z) * z)));
}

// The idle facing fan [orig: Entity_UpdateInfantryAI @0x4BD905..0x4BE07A]. An
// idle body (cover_idle 163, idle_2 44 or its look 126) on a def without
// LANDABLE, and without Flags 0x40000, turns away from the same-team pool-0
// bodies it sees close by and from what blocks it; it writes no move goal.
void idle_facing(AiSystem &ai, AiEntity &e, World &world, const Entity &self) {
    InfantryState &inf = e.inf;
    if (inf.anim_state != anim_state::kCoverIdle && inf.anim_state != anim_state::kIdle2 &&
            inf.anim_state != anim_state::kIdle2Look)
        return; // [orig: @0x4BD905..0x4BD91A]
    if ((self.item_attrib & def::DEF_ITEM_ATTRIB_LANDABLE) != 0 ||
            ((self.flags | self.engine_flags) & 0x40000u) != 0)
        return; // [orig: @0x4BD920..0x4BD93B]
    int32_t pos[3];
    position_of(ai, self, pos);
    int32_t mean = 0;
    int32_t count = 0;
    // A NOWEAPON def skips the teammate walk and the detour clear
    // [orig: @0x4BD943..0x4BD956].
    if ((self.item_attrib & def::DEF_ITEM_ATTRIB_NOWEAPON) == 0) {
        inf.path_state = 0;
        // Each teammate within 3.5 u per axis, 1.5 u up or down and 3 u planar,
        // seen through the entity LOS at height 0x4000, folds its bearing away
        // from it into a running mean. [orig: the pool-0 walk
        // @0x4BD990..0x4BDA67; `mean -= (mean - b) / k` @0x4BDA6D..0x4BDA9D]
        int32_t k = 1;
        world.registry.for_each_in_pool(0, [&](const Entity &other) {
            if (other.item_id == 0 ||
                    ((other.flags | other.engine_flags) & kEntityFlagDead) != 0 ||
                    other.handle == self.handle || other.team != self.team)
                return;
            int32_t at[3];
            position_of(ai, other, at);
            const int32_t dx = io::bam_sub(pos[0], at[0]);
            const int32_t dy = io::bam_sub(pos[1], at[1]);
            if (io::bam_abs(dx) > 0x38000 || io::bam_abs(dy) > 0x38000 ||
                    io::bam_abs(io::bam_sub(at[2], pos[2])) > 0x18000 ||
                    distance_of(dx, dy, 0) > 0x30000 ||
                    !infantry_entity_los(ai, world, self.handle, other.handle, pos, at, 0x4000,
                                         false))
                return;
            mean = io::bam_sub(mean, io::bam_sub(mean, bearing_to(dx, dy)) / k);
            ++count;
            ++k;
        });
    }
    // One probe: `range` out along `angle` (the Q22 trig of dbl_7C3608 /
    // dbl_7C3600), a quarter unit up, through the self entity LOS.
    const auto clear = [&](int32_t angle, int32_t range, int32_t height_offset) {
        const double radians = static_cast<double>(angle) * 1.4629627251502471e-9;
        const int32_t s = static_cast<int32_t>(std::sin(radians) * 4194304.0);
        const int32_t c = static_cast<int32_t>(std::cos(radians) * 4194304.0);
        const int32_t end[3] = {
            io::bam_add(pos[0], static_cast<int32_t>((static_cast<int64_t>(c) * range) >> 22)),
            io::bam_add(pos[1], static_cast<int32_t>((static_cast<int64_t>(s) * range) >> 22)),
            io::bam_add(pos[2], 0x4000)};
        return infantry_entity_los(ai, world, self.handle, self.handle, pos, end, height_offset,
                                   false);
    };
    // The fan around the mean, fixed before the first probe: +-90, 180, +-45,
    // +-135 degrees [orig: @0x4BDAA6..0x4BDAF1].
    const int32_t around[7] = {
        io::bam_add(mean, 0x3FFFFFC0), io::bam_sub(mean, 0x3FFFFFC0),
        io::bam_add(mean, 0x7FFFFF80), io::bam_add(mean, 0x1FFFFFE0),
        io::bam_sub(mean, 0x1FFFFFE0), io::bam_add(mean, 0x5FFFFFA0),
        io::bam_sub(mean, 0x5FFFFFA0)};
    // The mean itself, 3 u out at height 0x8000: clear ends the fan; blocked it
    // turns the mean half a turn [orig: @0x4BDB3C..0x4BDBC6]. Each later probe
    // (2 u, height 0x4000) found blocked folds its reverse bearing in
    // [orig: @0x4BDBED..0x4BE071].
    if (!clear(mean, 0x30000, 0x8000)) {
        ++count;
        mean = io::bam_add(mean, 0x7FFFFF80 / count);
        for (const int32_t angle : around) {
            if (clear(angle, 0x20000, 0x4000)) continue;
            ++count;
            mean = io::bam_add(mean, io::bam_add(io::bam_sub(angle, mean), 0x7FFFFF80) / count);
        }
    }
    if (count != 0) inf.target_heading = mean; // [orig: @0x4BE073..0x4BE07A]
}

int32_t look_step(int32_t difference, int shift, int32_t positive_threshold,
                  int32_t limit) {
    const int32_t step = io::bam_sar(io::bam_add(difference, 1 << (shift - 1)), shift);
    // The positive comparisons really are 16 times the stored clamp. Do not
    // replace this with symmetric clamp(): the instruction immediates differ.
    return step > positive_threshold ? limit : std::max(step, -limit);
}

} // namespace

void infantry_attention_think(AiSystem &ai, AiEntity &e, World &world, uint32_t key) {
    InfantryState &inf = e.inf;
    Entity *body = world.registry.get(e.handle);
    if (body == nullptr) return;
    infantry_ride_link(world, *body); // [orig: @0x4BD87E..0x4BD905]
    idle_facing(ai, e, world, *body); // [orig: @0x4BD905..0x4BE07A]
    const Entity *self = body;
    if (ai.root_motion == nullptr) return;
    const auto available = [&](int state) {
        return ai.root_motion->has_clip(inf.adm_id, state);
    };
    if (inf.anim_state == 147 && !available(147)) inf.store_body_animation(43);
    if (inf.anim_state >= 0 && inf.anim_state < kInfantryAnimStateCount)
        world.facials.automatic_expression(*self, kInfantryFacialExpressions[inf.anim_state]);

    // The scripted voice speaker rescans at the 32-tick perception phase, not on
    // every think: the key & 0x1F word the perception gate stores is re-read by
    // the speaker arm. [orig: Entity_UpdateInfantryAI phase @0x4BBE4A, gate
    // @0x4BE0CA..0x4BE0E8]
    const bool speaker = world.script.voice.speaker() == e.handle;
    if ((key & 255u) == 0 || (speaker && (key & 31u) == 0)) {
        const int32_t radius = std::min(e.slot.f[17], 20 * 65536);
        int32_t eye[3];
        eye_of(ai, *self, eye);
        int32_t best_score = 0;
        inf.head_look_target = {};
        world.registry.for_each_in_pool(0, [&](const Entity &candidate) {
            const uint32_t flags = candidate.flags | candidate.engine_flags;
            if (candidate.item_id == 0 || (flags & kEntityFlagCarried) != 0 ||
                    candidate.handle == e.handle ||
                    (candidate.handle == world.cached.local_player &&
                     world.rules.ai_rules_skip_local_player)) return;
            int32_t point[3];
            eye_of(ai, candidate, point);
            const int32_t dx = io::bam_sub(point[0], eye[0]);
            const int32_t dy = io::bam_sub(point[1], eye[1]);
            const int32_t dz = io::bam_sub(point[2], eye[2]);
            if (io::bam_abs(dx) > radius || io::bam_abs(dy) > radius ||
                    io::bam_abs(dz) > radius) return;
            const int32_t distance = distance_of(dx, dy, dz);
            // Axis bounds admit diagonal points outside the sphere. The retail
            // unsigned subtraction/shift gives those points a large score.
            int32_t score = static_cast<int32_t>((uint32_t(radius) - uint32_t(distance)) >> 16);
            if (speaker && e.team != 0 && candidate.team == e.team && inf.damage_timer == 0)
                score += 8;
            if (candidate.handle == world.cached.local_player) score += 2;
            const int32_t bearing = bearing_to(dx, dy);
            const AiEntity *candidate_body = ai.for_handle(candidate.handle);
            const int32_t candidate_yaw = candidate_body ? candidate_body->heading
                    : bam_heading_from_mission_yaw_deg(candidate.yaw);
            if (distance < 2 * 65536 &&
                    io::bam_abs(io::bam_add(io::bam_sub(bearing, candidate_yaw),
                                           INT32_MIN)) < 298261600)
                score += 4;
            if (candidate.handle == inf.last_look_target) score -= 12;
            if (candidate.handle == inf.previous_look_target) return;
            score -= 12;
            if (!ai.line_of_sight_clear(world, eye, point, e.handle, candidate.handle)) return;
            if ((flags & kEntityFlagDead) != 0 && inf.damage_timer == 0) {
                inf.damage_timer = 25;
                score += 4;
            }
            if (e.team != 0 && candidate.team != 0 && candidate.team != e.team &&
                    inf.damage_timer == 0) {
                if ((e.slot.f[1] & 1) == 0) {
                    position_of(ai, candidate, inf.aim_point);
                    inf.ai_focus = candidate.handle;
                    inf.damage_timer = 10;
                }
                score += 4;
            }
            score += static_cast<int32_t>((uint32_t(e.net_id) + (key >> 8) - uint32_t(candidate.net_id)) & 7u);
            if (score > best_score && score >= 4 &&
                    io::bam_abs(io::bam_sub(bearing, inf.body_heading)) <= 835132480) {
                best_score = score;
                inf.head_look_target = candidate.handle;
            }
        });
        inf.previous_look_target = inf.last_look_target;
        inf.last_look_target = inf.head_look_target;
        if (const Entity *target = world.registry.get(inf.head_look_target)) {
            auto &relations = world.script.relations;
            relations.set_group_group(TriggerRelations::kSees, self->group_id, target->group_id);
            relations.set_single_group(TriggerRelations::kSees, self->net_id, target->group_id);
            relations.set_group_single(TriggerRelations::kSees, self->group_id, target->net_id);
            relations.set_single_single(TriggerRelations::kSees, self->net_id, target->net_id);
        }
    }

    bool overridden = inf.aim_override;
    if (!overridden) {
        inf.aim_heading = inf.body_heading;
        inf.aim_pitch = e.body_pitch;
    }
    const AiEntity *parent = self->mounted ? ai.for_handle(self->mount_target) : nullptr;
    if (parent != nullptr && is_vehicle_control_seat(self->mount_type) &&
            parent->brain.f[136] != 0) overridden = true;
    if (overridden ||
            (inf.anim_state == 140 && !self->mounted && !available(141)) ||
            (inf.anim_state == 43 && !available(125)) ||
            (inf.anim_state == 44 && !available(126))) return;

    const Entity *target = world.registry.get(inf.head_look_target);
    if (target == nullptr) {
        inf.head_look_target = {};
        return;
    }
    int32_t from[3], to[3];
    eye_of(ai, *self, from);
    eye_of(ai, *target, to);
    const int32_t jitter = 8 * int32_t(key & 0xC0u);
    const int32_t dx = io::bam_sub(io::bam_add(to[0], jitter - 1024), from[0]);
    const int32_t dy = io::bam_sub(io::bam_add(to[1], jitter - 512), from[1]);
    const int32_t dz = io::bam_sub(io::bam_add(to[2], 4 * int32_t(key & 0x180u) - 1024), from[2]);
    const int32_t bearing = bearing_to(dx, dy);
    if (distance_of(dx, dy, dz) >= 25 * 65536 ||
            io::bam_abs(io::bam_sub(bearing, inf.body_heading)) >= 894784800) {
        inf.head_look_target = {};
        return;
    }
    inf.aim_heading = bearing;
    inf.aim_pitch = std::clamp(bearing_to(distance_of(dx, dy, 0), dz), -357913920, 357913920);
    inf.aim_valid = false;
    inf.aim_established = true;
    // Nearby characters looking back at this NPC select the phased social
    // expression; this is independent of the timed WAC override.
    // [orig: Entity_UpdateInfantryAI @0x4BE6AE..0x4BE729]
    const AiEntity *target_body = ai.for_handle(target->handle);
    const int32_t target_heading = target_body ? target_body->heading :
            static_cast<int32_t>(int64_t(90 - target->yaw) * 11930464);
    if (distance_of(dx, dy, dz) < 2 * 65536 &&
            io::bam_abs(io::bam_sub(io::bam_sub(bearing, target_heading), INT32_MIN)) < 298261600) {
        const uint32_t phase = key & 0x180u;
        if (phase == 0x100) world.facials.automatic_expression(*self, 5);
        if (phase == 0x180) world.facials.automatic_expression(*self, 7);
        if (phase == 0x80) world.facials.automatic_expression(*self, 6);
    }
    for (const auto pair : {std::pair<int, int>{140, 141}, {43, 125}, {44, 126}}) {
        if (!available(pair.second)) continue;
        if (inf.anim_state == pair.first) inf.store_body_animation(pair.second);
        if (inf.anim_pending == pair.first) inf.anim_pending = pair.second;
    }
}

// [orig: mounted @0x4BEF57..0x4BEF97; on foot @0x4BEB18..0x4BEBD9]
void infantry_look_tick(const InfantryState &inf, int32_t &heading, int32_t &pitch,
                        bool mounted) {
    const int32_t yaw_error = io::bam_sub(inf.aim_heading, heading);
    const int32_t pitch_error = io::bam_sub(inf.aim_pitch, pitch);
    if (mounted) {
        heading = io::bam_add(heading, std::clamp(
                io::bam_sar(io::bam_add(yaw_error, 2), 2), -0x02000000, 0x02000000));
        pitch = io::bam_add(pitch, io::bam_sar(io::bam_add(pitch_error, 4), 3));
        return; // The parent configuration selects the mounted arc clamp.
    }
    if (inf.aim_valid) {
        heading = io::bam_add(heading, look_step(yaw_error, 2, 0x1E000000, 0x1E00000));
        pitch = io::bam_add(pitch, look_step(pitch_error, 2, 0x1E000000, 0x1E00000));
    } else {
        heading = io::bam_add(heading, look_step(yaw_error, 3, 0xE000000, 0xE00000));
        pitch = io::bam_add(pitch, look_step(pitch_error, 3, 0xC000000, 0xC00000));
    }
    const int32_t twist = io::bam_sub(heading, inf.body_heading);
    if (twist > 0x40000000) heading = io::bam_add(inf.body_heading, 0x40000000);
    if (twist < -0x40000000) heading = io::bam_sub(inf.body_heading, 0x40000000);
}

} // namespace opennova::world
