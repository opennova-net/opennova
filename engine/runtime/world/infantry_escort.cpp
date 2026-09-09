// Organic escort approach and medic dragging.
// [orig: Entity_UpdateInfantryAI @0x4B9910; HeliLift_UpdateSlotState @0x451730]
#include <runtime/world/infantry_internal.h>
#include <runtime/world/angle.h>
#include <runtime/world/world.h>
#include <runtime/world/pose_provider.h>
#include <base/io/bam.h>

#include <algorithm>
#include <cmath>

namespace opennova::world {
namespace {
int32_t heading_of(const World &world, const Entity &entity) {
    const AiEntity *body = world.ai.for_handle(entity.handle);
    return body ? body->heading : bam_heading_from_mission_yaw_deg(entity.yaw);
}
void anchor_of(World &world, const Entity &entity, SkeletalAnchor anchor, int32_t out[3]) {
    if (world.pose_provider &&
            world.pose_provider->resolve_skeletal_anchor(world, entity.handle, anchor, out)) return;
    if (const AiEntity *body = world.ai.for_handle(entity.handle)) {
        std::copy_n(body->pos, 3, out);
    } else {
        out[0] = static_cast<int32_t>(entity.position.x * 65536.0f);
        out[1] = static_cast<int32_t>(entity.position.y * 65536.0f);
        out[2] = static_cast<int32_t>(entity.position.z * 65536.0f);
    }
}
void offset_goal(int32_t goal[3], int32_t heading, int32_t amount) {
    const double radians = heading / io::kBamPerRadian;
    const int32_t c = static_cast<int32_t>(std::cos(radians) * 4194304.0);
    const int32_t s = static_cast<int32_t>(std::sin(radians) * 4194304.0);
    goal[0] = io::bam_add(goal[0], static_cast<int32_t>((int64_t(c) * amount) >> 22));
    goal[1] = io::bam_add(goal[1], static_cast<int32_t>((int64_t(s) * amount) >> 22));
}
int32_t weighted_distance(const AiEntity &e, const int32_t goal[3]) {
    const int32_t dx = io::bam_sub(goal[0], e.pos[0]);
    const int32_t dy = io::bam_sub(goal[1], e.pos[1]);
    const int32_t dz = io::bam_sar(io::bam_sub(goal[2], e.pos[2]), 3);
    return static_cast<int32_t>(std::min(2147418112.0,
            std::sqrt(double(dx) * dx + double(dy) * dy + double(dz) * dz)));
}
void helicopter_approach(int32_t goal[3], int32_t heading, int32_t distance) {
    const bool far = distance > 0x80000;
    offset_goal(goal, io::bam_sub(heading, 1073741760), far ? 0x40000 : 0x20000);
    if (far) goal[2] = io::bam_sub(goal[2], 163840);
}
} // namespace

// These identities are assigned by the teammate spawn helpers, not mission names.
// Far offsets retain the original unoffset distance; only the near legs recompute.
// [orig: Entity_UpdateInfantryAI @0x4BB62C..0x4BBD87]
void infantry_escort_goal(AiEntity &e, World &world, const Entity &target,
                         int32_t goal[3], int32_t &radius, int32_t &distance) {
    const Entity *self = world.registry.get(e.handle);
    if (self == nullptr) return;
    const int32_t heading = heading_of(world, target);
    if (self->net_id == 12000) {
        e.inf.aim_pitch = 0;
        radius = 81920;
        if (target.net_id == 11000) {
            if (distance > 0x40000) {
                helicopter_approach(goal, heading, distance);
            } else {
                offset_goal(goal, io::bam_add(heading, 1073741760), 49152);
                goal[2] = io::bam_sub(goal[2], 114688);
                distance = weighted_distance(e, goal);
            }
        } else {
            int32_t head[3];
            anchor_of(world, target, SkeletalAnchor::Head, head);
            goal[0] = head[0];
            goal[1] = head[1]; // Z remains the original order target's Z.
            radius = 24576;
            distance = weighted_distance(e, goal);
        }
    }
    if (self->net_id == 12001) {
        radius = 0x10000;
        if (target.net_id == 11000) {
            if (distance > 0x40000) {
                helicopter_approach(goal, heading, distance);
            } else {
                goal[2] = io::bam_sub(goal[2], 98304);
                if (distance < 122880) {
                    offset_goal(goal, heading, 122880);
                    distance = weighted_distance(e, goal);
                    radius = 106496;
                }
            }
        }
        if (distance < radius) e.inf.target_heading = heading;
    }
}

bool infantry_is_dragger(const AiEntity &e, const World &world) {
    const Entity *self = world.registry.get(e.handle);
    return self && self->dragger == self->handle &&
            self->dragger_spawn_id == self->registry_spawn_id;
}

// Called before corpse age/respawn. Only XY follows the hand; gravity retains Z.
// [orig: Entity_UpdateInfantryAI @0x4B9D60..0x4B9E41]
bool infantry_drag_corpse(AiEntity &e, World &world) {
    Entity *self = world.registry.get(e.handle);
    if (self == nullptr) return false;
    self->flags &= ~kEntityFlagMounted;
    self->engine_flags &= ~kEntityFlagMounted;
    const Entity *dragger = world.registry.get(self->dragger);
    if (dragger == nullptr || dragger->registry_spawn_id != self->dragger_spawn_id ||
            dragger->item_id == 0 || ((dragger->flags | dragger->engine_flags) & kEntityFlagDead) ||
            world.ai.root_motion == nullptr ||
            !world.ai.root_motion->has_clip(e.inf.adm_id, 139)) return false;
    int32_t hand[3], head[3];
    anchor_of(world, *dragger, SkeletalAnchor::HeldWeapon, hand);
    anchor_of(world, *self, SkeletalAnchor::Head, head);
    e.pos[0] = io::bam_add(e.pos[0], io::bam_sub(hand[0], head[0]));
    e.pos[1] = io::bam_add(e.pos[1], io::bam_sub(hand[1], head[1]));
    self->position = {e.pos[0] / 65536.0f, e.pos[1] / 65536.0f, e.pos[2] / 65536.0f};
    const AiEntity *body = world.ai.for_handle(dragger->handle);
    e.inf.target_heading = e.inf.aim_heading = body ? body->inf.body_heading
            : heading_of(world, *dragger);
    e.inf.aim_valid = false; // aimFlag [orig: @0x4B9E30]; the look chase takes its slow arm
    e.inf.aim_established = true;
    e.inf.anim_pending = 0; // [orig: @0x4B9E41]
    return true;
}

} // namespace opennova::world
