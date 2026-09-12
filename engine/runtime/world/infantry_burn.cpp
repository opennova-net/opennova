#include <runtime/world/infantry_burn.h>
#include <runtime/world/world.h>
#include <base/io/bam.h>
#include <base/io/fixed.h>
#include <cmath>

namespace opennova::world {
// [orig: Entity_ApplyCollisionForce @0x4AF4A0]
void apply_infantry_burn(World &world, Entity &target, uint8_t hit_type,
        const Vec3 &source, EntityHandle attacker) {
    const uint32_t flags = target.flags | target.engine_flags;
    if (hit_type == 0 || (flags & kEntityFlagDead) != 0) return;
    AiEntity *body = world.ai.for_handle(target.handle);
    if (body == nullptr) return;
    InfantryState &inf = body->inf;
    inf.burn_state = hit_type;
    if ((flags & kEntityFlagPlayer) != 0) {
        inf.idle_counter = 1;
        return;
    }
    const int32_t dx = io::bam_sub(body->pos[0], static_cast<int32_t>(source.x * io::kFp16One));
    const int32_t dy = io::bam_sub(body->pos[1], static_cast<int32_t>(source.y * io::kFp16One));
    const int32_t bearing = static_cast<int32_t>(static_cast<uint32_t>(
            static_cast<int64_t>(std::atan2(double(dy), double(dx)) * io::kBamPerRadian)));
    const int multiplier = io::bam_abs(io::bam_sub(bearing, body->heading)) < 0x3FFFFFC0 ? 3 : 4;
    const Entity *owner = world.registry.get(attacker);
    // Retail assumes a valid owner here. An ownerless embedder event uses the
    // ordinary duration; it cannot establish a same-command-group reaction.
    const bool same_group = owner != nullptr && owner->group_id == target.group_id;
    inf.combat_move_timer = ((same_group ? 2 : 48) * multiplier) >> 2;
}

// [orig: org1 @0x4BBF8F; org2 @0x4B70DE]
int select_infantry_burn(InfantryState &inf, const IRootMotionSource *motion,
        bool player, uint32_t tick) {
    int selected = player ? inf.anim_state : 0;
    if (inf.burn_state == 0) return selected;
    if (inf.burn_state <= 4) {
        const int state = 110 + inf.burn_state;
        if (motion != nullptr && motion->has_clip(inf.adm_id, state)) selected = state;
        else if (!player) inf.burn_state = 0;
    }
    if (!player || (tick & 15u) == 0) {
        int32_t &timer = player ? inf.idle_counter : inf.combat_move_timer;
        if (timer != 0) timer = io::bam_sub(timer, 1);
        else inf.burn_state = 0;
    }
    return selected;
}
} // namespace opennova::world
