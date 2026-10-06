// The wire-fed remote body's footstep/foley consume — witness map in
// world/wire_body_sound.h; the authority sibling is
// AiSystem::infantry_anim_sound_pass (world/infantry_sound.cpp).
#include <runtime/world/wire_body_sound.h>

#include <runtime/anim/anim_event_bits.h>
#include <runtime/audio/footstep_slot.h>
#include <runtime/terrain_query/surface_type_map.h>
#include <runtime/world/infantry_sound.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <string>

namespace opennova::world {

void wire_body_slot_sounds(World &world, const uint32_t *words, int count,
                           int32_t capsule_bottom, int32_t item_id,
                           uint16_t character_id, uint16_t source_handle,
                           bool on_entity, const int32_t body[3]) {
    if (words == nullptr || count <= 0) return;
    // Only player rows carry the packed avatar identity; NPC rows keep the
    // primary profile. [orig: Entity_GetProfileSlotSound @0x528300, the
    // female byte @0x52831c — player-only, D-SND-12]
    const bool female =
            character_id != 0 && world.tables.character_traits.is_female(character_id);
    const auto emit = [&](int slot, const int32_t pos[3]) {
        const std::string *set = audio::organic_slot_set(
                world.tables.sound_profiles, world.tables.organic_sound_profiles,
                item_id, female, slot);
        if (set == nullptr) return; // the resolved-id-0 no-op
        SoundSlotEvent sev;
        sev.source_handle = source_handle;
        sev.pos[0] = pos[0];
        sev.pos[1] = pos[1];
        sev.pos[2] = pos[2];
        sev.slot = static_cast<uint8_t>(slot);
        std::snprintf(sev.set_name, sizeof(sev.set_name), "%s", set->c_str());
        world.out.slot_sounds.push_back(sev);
    };
    // FOLEY first, at the body origin — each body's foley block precedes its
    // foot block — then the feet, dipped to FOOT level by the frame's capsule
    // bottom, through the shared slot pick (anim_event_sounds, the order the
    // authority body plays them in).
    // [orig: org1 @0x4bf169-0x4bf2b0; org2 @0x4b76f1-0x4b78a8; the dip
    //  @0x4b77d3]
    const int32_t feet[3] = {body[0], body[1], body[2] - capsule_bottom};
    for (int i = 0; i < count; ++i) {
        const uint32_t ev = words[i];
        if (ev == 0u) continue;
        const bool any_foot = (ev & (anim::kAnimEventFootLeft | anim::kAnimEventFootRight)) != 0u;
        const int32_t surface = any_foot
                ? terrain::surface_type_at_fixed(world.tables.surface_map, feet[0], feet[1]) : 0;
        AnimEventSound sounds[kAnimEventSoundMax];
        const int played = anim_event_sounds(ev, feet[2], world.env.water_z, on_entity, surface, sounds);
        for (int s = 0; s < played; ++s) emit(sounds[s].slot, sounds[s].foot < 0 ? body : feet);
    }
}

} // namespace opennova::world
