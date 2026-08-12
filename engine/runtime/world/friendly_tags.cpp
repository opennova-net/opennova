#include "world/friendly_tags.h"

#include <algorithm>
#include <cstdint>

#include "world/world.h"

namespace opennova::world {

void collect_friendly_tags(World &world, const Entity &local,
                           std::vector<FriendlyTagSource> &out) {
    world.registry.for_each([&](const Entity &e) {
        // The pool-0 organic walk; players ride the slot walk in retail (the
        // documented MP residue) [orig: the pool-0 loop @0x5a44b0 skips
        // Flags & 0x100 @0x5a44bc].
        if (e.kind != EntityKind::Organic) return;
        if ((e.flags & kEntityFlagPlayer) != 0) return;
        // The drawer's entry bails: never the local player, never a dead /
        // movement-locked entity, and only with a resolved item def
        // [orig: @0x5a39df entity == playerEntity; @0x5a39eb Flags & 1;
        //  @0x5a39fb itemDef == NULL].
        if (e.handle == local.handle) return;
        if (!e.alive || (e.flags & 1u) != 0) return;
        if (!e.has_item_def) return;
        // Team gate: neutral or the local team. The enemy magenta leg is
        // server-granted spectator state (g_enemyTagsVisible), unported
        // [orig: @0x5a44c7..0x5a44f8; the @0x5a3c7d enemy bail].
        if (e.team != 0 && e.team != local.team) return;

        FriendlyTagSource src;
        src.entity = e.handle;
        src.net_id = e.net_id;
        src.position = e.position;
        // The anchor's eye lift rides along: z + entity[+116] + 0x4000
        // [orig: HUD_DrawEntityLabel @0x5a3a84..0x5a3a98].
        src.eye_offset_z = e.eye_offset_z;
        src.name = e.display_name;
        src.player = false;
        // health<<16 / max, clamped to 1.0 — the difficulty-scaled max
        // (Entity_GetMaxHealthWithDifficulty) is the base def hp here; the
        // difficulty term is the record's residue [orig: @0x5a3b91..0x5a3bb8,
        // clamp mirror of HUD_BuildEntityInfo @0x4b87d1].
        const int64_t max_health = std::max(1, e.health_max);
        const int64_t ratio =
                (static_cast<int64_t>(std::max(0, e.health)) << 16) / max_health;
        src.health_ratio_fp16 =
                static_cast<int32_t>(std::min<int64_t>(ratio, 0x10000));
        out.push_back(src);
    });
}

} // namespace opennova::world
