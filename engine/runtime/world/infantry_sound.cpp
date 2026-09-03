// The infantry SOUND leg of the org1/org2 body updaters: the per-slot profile
// resolve (emit_slot_sound) and the anim-event consumer that turns the six
// foley bits and the two footstep bits into SoundSlotEvents. Split from
// infantry.cpp by leg (the size ratchet), the same way infantry_ladder.cpp
// carries the climb legs; the witnesses stay inline at each function.
// [orig: Entity_UpdateInfantryAI sound block @0x4bf144-0x4bf2b0;
//  Entity_UpdateInfantryPlayerBody sound block @0x4b76e6-0x4b78a8;
//  Entity_GetProfileSlotSound @0x528300]

#include <cstdio>

#include <runtime/audio/footstep_slot.h>
#include <base/io/bam.h>
#include <runtime/terrain_query/height_field.h>

#include <runtime/world/ai.h>
#include <runtime/world/world.h>
#include <cstdint>
#include <string>

namespace opennova::world {

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
            : world.tables.sound_profiles.find("default");
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
    // body on EVEN [orig: org1 `and eax,1; jz skip` @0x4bf144-0x4bf156; org2
    // `test current_tick,1; jnz skip` @0x4b76e6 (var = current_tick @0x4b4147)].
    if (inf.is_local_player ? ((logic_tick & 1u) != 0) : ((logic_tick & 1u) == 0)) return;
    const uint32_t ev = inf.last_events;
    if (ev == 0) return; // [orig: org1 whole-block skip @0x4bf161-0x4bf163]

    // The six anim-driven foley sounds, bit order 0x20..0x400 -> SSAudio1..6
    // (JO persons author prone rolls, swim strokes, gear rustle here), at the
    // entity origin [orig: org1 @0x4bf169-0x4bf23e; org2 @0x4b76f1-0x4b77c6].
    for (int i = 0; i < 6; ++i) {
        if ((ev & (0x20u << i)) != 0)
            emit_slot_sound(world, e, audio::kSlotAudio1 + i, e.pos);
    }

    // Footsteps: bit 0x1 = left, 0x2 = right. The sound fires at FOOT level —
    // pos.z dipped by the root-motion frame's capsule bottom (the same value
    // the collision capsule uses; the original subtracts it in place, plays,
    // and restores) — and the slot picks by, in order: feet under the water
    // plane -> standing on an entity -> terrain surface 3 (snow) -> ground.
    // [orig: org1 @0x4bf23e-0x4bf2b0; org2 @0x4b77c6-0x4b78a8; the dip slot is
    // the AnimMap out[3] stack cell both bodies pass to the anim update]
    const Entity *went = world.registry.get(e.handle);
    for (int foot = 0; foot < 2; ++foot) {
        if ((ev & (foot == 0 ? 0x1u : 0x2u)) == 0) continue;
        const int32_t pos[3] = {e.pos[0], e.pos[1], e.pos[2] - capsule_bottom};
        // The witnessed test order lives in audio::footstep_slot, shared with
        // the wire-fed remote body channel so both consume one implementation.
        // The on-entity read is last tick's link: this pass runs BEFORE this
        // tick's resolve, the same order as org1 (sound block @0x4bf23e
        // precedes the resolve tail @0x4bf7b8+). Mounted bodies never reach
        // here — seat clips author no foot-event bits.
        const int slot = audio::footstep_slot(
                pos[2], world.env.water_z,
                went != nullptr && went->ground_target.valid(),
                terrain::surface_type_at_fixed(world.tables.surface_map, pos[0], pos[1]),
                foot);
        emit_slot_sound(world, e, slot, pos);
    }
}

} // namespace opennova::world
