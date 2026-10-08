// The infantry SOUND leg of the org1/org2 body updaters: the per-slot profile
// resolve (emit_slot_sound) and the anim-event consumer that turns the six
// foley bits and the two footstep bits into SoundSlotEvents. Split from
// infantry.cpp by leg (the size ratchet), the same way infantry_ladder.cpp
// carries the climb legs; the witnesses stay inline at each function.
// [orig: Entity_UpdateInfantryAI sound block @0x4bf144-0x4bf2b0;
//  Entity_UpdateInfantryPlayerBody sound block @0x4b76e6-0x4b78a8;
//  Entity_GetProfileSlotSound @0x528300]

#include <cstdio>

#include <runtime/anim/anim_event_bits.h>
#include <runtime/audio/footstep_slot.h>
#include <base/io/bam.h>
#include <runtime/terrain_query/height_field.h>

#include <runtime/world/ai.h>
#include <runtime/world/infantry_sound.h>
#include <runtime/world/world.h>
#include <cstdint>
#include <string>

namespace opennova::world {

// The player's stance latch is independent of clip selection and anim-event
// foley. Changing to the unnamed combined state still updates the latch.
// parent_has_definition is the +0x16C MOUNT parent's ItemDef (parentEntity
// gate @0x4B41A2..0x4B41C0, prone clear @0x4B4709..0x4B471B), never the
// +0x28 ground link.
// [orig: Entity_UpdateInfantryPlayerBody @0x4B703D..0x4B709D;
//  global sound rows TO_CROUCH/TO_PRONE/TO_STAND @0x82FFB0/0x82FFD4/0x82FFF8]
void emit_stance_change_sound(World &world, uint16_t source, const int32_t pos[3],
        uint8_t &previous, uint8_t stance_bits, uint32_t flags, bool parent_has_definition) {
    const bool prone = (stance_bits & 1u) != 0 &&
            (flags & 0x10A040u) == 0 && !parent_has_definition;
    const uint8_t current = static_cast<uint8_t>(((stance_bits >> 1) & 1u) + (prone ? 2 : 0));
    if (current == previous) return;
    previous = current;
    const char *name = current == 0 ? "TO_STAND" : current == 1 ? "TO_CROUCH" :
                       current == 2 ? "TO_PRONE" : nullptr;
    if (name == nullptr) return;
    SoundSlotEvent sound;
    sound.source_handle = source;
    for (int axis = 0; axis < 3; ++axis) sound.pos[axis] = pos[axis];
    std::snprintf(sound.set_name, sizeof(sound.set_name), "%s", name);
    world.out.slot_sounds.push_back(sound);
}


void AiSystem::emit_slot_sound(World &world, const AiEntity &e, int slot, const int32_t pos[3]) {
    if (slot < 0 || slot >= audio::kSoundProfileSlotCount) return;
    const auto &entries = world.tables.sound_profiles.entries();
    if (entries.empty()) return;
    // An unresolved binding falls back to the "default" profile, which itself
    // falls back to the first profile when no "default" exists — the alloc-time
    // seed + the find-miss base return [orig: ItemDef_AllocateWithDefaults
    // @0x49e3f5 seeds FindSlotByName("default"); @0x526e30 miss -> base].
    int16_t profile_index = e.profile.sound_profile;
    const Entity *source = world.registry.get(e.handle);
    // Only player entities carry the packed avatar identity. NPC women author
    // their own primary item profile and must not be reinterpreted through a
    // coincident minimap/net id. Unknown character ids keep the primary.
    // [orig: Entity_GetProfileSlotSound @0x528300, female byte @0x52831c]
    if (source != nullptr && source->player_class != 0 &&
        world.tables.character_traits.is_female(source->minimap_net_id))
        profile_index = e.profile.sound_profile_female;
    const audio::SoundProfile *p =
        (profile_index >= 0 && static_cast<size_t>(profile_index) < entries.size())
            ? &entries[profile_index]
            : audio::item_sound_profile(entries, nullptr);
    if (p == nullptr) return;
    const std::string &set = p->set_names[slot];
    if (set.empty()) return; // the resolved-id-0 no-op [orig: table[slot] == 0]
    SoundSlotEvent ev;
    ev.source_handle = e.handle.packed;
    ev.pos[0] = pos[0];
    ev.pos[1] = pos[1];
    ev.pos[2] = pos[2];
    ev.slot = static_cast<uint8_t>(slot);
    std::snprintf(ev.set_name, sizeof(ev.set_name), "%s", set.c_str());
    world.out.slot_sounds.push_back(ev);
}

void AiSystem::infantry_anim_sound_pass(AiEntity &e, World &world, uint32_t logic_tick,
                                        int32_t capsule_bottom) {
    InfantryState &inf = e.inf;
    // Opposite tick halves: the NPC updater consumes on ODD ticks, the player
    // body on EVEN (anim_sound_tick, witnessed at its declaration).
    if (!anim_sound_tick(logic_tick, inf.is_local_player)) return;
    const uint32_t ev = inf.last_events;
    if (ev == 0) return; // [orig: org1 whole-block skip @0x4bf161-0x4bf163]

    // The six anim-driven foley sounds (JO persons author prone rolls, swim
    // strokes, gear rustle here) at the entity origin, then the footsteps at
    // FOOT level — pos.z dipped by the root-motion frame's capsule bottom (the
    // same value the collision capsule uses; the original subtracts it in
    // place, plays, and restores) — each slot picked by, in order: feet under
    // the water plane -> standing on an entity -> terrain surface 3 (snow) ->
    // ground (anim_event_sounds over audio::footstep_slot, the one order the
    // wire-fed remote body channel and the editor's clip preview share).
    // [orig: org1 @0x4bf169-0x4bf2b0; org2 @0x4b76f1-0x4b78a8; the dip slot is
    // the AnimMap out[3] stack cell both bodies pass to the anim update]
    // The on-entity read is last tick's link: this pass runs BEFORE this
    // tick's resolve, the same order as org1 (sound block @0x4bf23e precedes
    // the resolve tail @0x4bf7b8+). Mounted bodies never reach here — seat
    // clips author no foot-event bits.
    const int32_t feet[3] = {e.pos[0], e.pos[1], e.pos[2] - capsule_bottom};
    const bool any_foot = (ev & (anim::kAnimEventFootLeft | anim::kAnimEventFootRight)) != 0;
    const Entity *went = any_foot ? world.registry.get(e.handle) : nullptr;
    const int32_t surface = any_foot
            ? terrain::surface_type_at_fixed(world.tables.surface_map, feet[0], feet[1]) : 0;
    AnimEventSound sounds[kAnimEventSoundMax];
    const int count = anim_event_sounds(ev, feet[2], world.env.water_z,
            went != nullptr && went->ground_target.valid(), surface, sounds);
    for (int i = 0; i < count; ++i)
        emit_slot_sound(world, e, sounds[i].slot, sounds[i].foot < 0 ? e.pos : feet);
}

int anim_event_sounds(uint32_t word, int32_t feet_z, int32_t water_z, bool on_entity,
                      int32_t surface_type, AnimEventSound out[kAnimEventSoundMax]) {
    int count = 0;
    // The foley block: bit order 0x20..0x400 -> SSAudio1..6 [orig: org1
    // @0x4bf169-0x4bf23e; org2 @0x4b76f1-0x4b77c6].
    for (int i = 0; i < anim::kAnimEventFoleyCount; ++i)
        if ((word & (anim::kAnimEventFoley1 << i)) != 0)
            out[count++] = AnimEventSound{audio::kSlotAudio1 + i, -1};
    // Then the feet, left before right, each through the one footstep pick
    // [orig: org1 @0x4bf23e-0x4bf2b0; org2 @0x4b77c6-0x4b78a8].
    for (int foot = 0; foot < 2; ++foot) {
        const uint32_t foot_bit = foot == 0 ? anim::kAnimEventFootLeft : anim::kAnimEventFootRight;
        if ((word & foot_bit) == 0) continue;
        out[count++] = AnimEventSound{
                audio::footstep_slot(feet_z, water_z, on_entity, surface_type, foot), foot};
    }
    return count;
}

} // namespace opennova::world
