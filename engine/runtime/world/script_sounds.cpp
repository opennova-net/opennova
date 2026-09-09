#include <runtime/world/world.h>

#include <cstdio>

namespace opennova::world {
namespace {
void play_at_entity(World &world, const Entity &entity, const std::string &name) {
    if (name.empty()) return;
    SoundSlotEvent sound;
    sound.source_handle = entity.handle.packed;
    if (const AiEntity *body = world.ai.for_handle(entity.handle)) {
        for (int axis = 0; axis < 3; ++axis) sound.pos[axis] = body->pos[axis];
    } else {
        sound.pos[0] = to_fixed(entity.position.x);
        sound.pos[1] = to_fixed(entity.position.y);
        sound.pos[2] = to_fixed(entity.position.z);
    }
    std::snprintf(sound.set_name, sizeof(sound.set_name), "%s", name.c_str());
    world.out.slot_sounds.push_back(sound);
}
} // namespace

// [orig: WacCmd_SoundSetToSsn @0x4F1DD0] A zero sound still returns success
// for a valid source. Health and ItemDef do not gate this entry.
bool EntityCommands::play_ssn_soundset(EntityTarget target, const std::string &name) {
    const Entity *entity = world_.registry.get(resolve_target(target));
    if (!entity || entity->item_id == 0) return false;
    play_at_entity(world_, *entity, name);
    return true;
}

// [orig: WacCmd_SoundToTarget @0x4F7F60] First pool-3 target marker,
// ItemDef id 6088 + WP_NUMBER. Shared full-volume positional entry @0x528E20.
int EntityCommands::sound_at_target(int32_t sound, const std::string &name, int32_t target) {
    const Entity *marker = script_target(target);
    if (!marker || sound == 0) return 1;
    play_at_entity(world_, *marker, name);
    return 0;
}

// [orig: WacCmd_Sound @0x4ED590 -> Sound_PlayTriggerSetScaled @0x527B90]
// No position, entity, 3D range gate or occlusion query. The audio consumer
// receives the already-resolved distance and raw bearing unchanged.
int EntityCommands::direct_sound(const std::string &name, int32_t distance, int32_t bearing) {
    if (!name.empty()) world_.out.script_sounds.push_back({name, distance, bearing});
    return 0;
}

} // namespace opennova::world
