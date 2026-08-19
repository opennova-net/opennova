#pragma once

#include <cstdint>
#include <string>

#include "audio/sound_profile.h"

namespace opennova::audio {

// THE FOOTSTEP SLOT PICK — which sound-profile slot one authored foot event
// resolves to, in retail's witnessed test order
// [orig: org2 @0x4b77c6-0x4b78a8; the sibling org1 block @0x4bf23e-0x4bf2b0].
//
// The tests run in this order and the FIRST match wins:
//   1. water — a nonzero water plane with the FEET under it (one slot for both
//      feet) [orig: Env_WaterHeightFixed @0x26C6454 read @0x4b77d3];
//   2. on-entity — the body is standing on another entity, i.e. its
//      `groundEntity` link (entity+0x28) is set [orig: the +0x28 store by
//      Entity_RaycastGroundHeightAndObject @0x525fd0 / @0x414370];
//   3. snow — charmap surface type 3 under the feet
//      [orig: Terrain_GetSurfaceTypeAtPosition @0x606510];
//   4. ground — everything else.
//
// `feet_z` is the body position ALREADY dipped to foot level by the frame's
// capsule bottom (the AnimMap out[3] cell both body updaters pass through);
// this function does not dip it. `water_z == 0` means "no water plane".
// `foot` is 0 = left, 1 = right; the water slot ignores it, matching retail.
//
// One implementation shared by both body channels: the authority/AI bodies
// (world::AiSystem::infantry_anim_sound_pass) and the wire-fed remote bodies.
inline int footstep_slot(int32_t feet_z, int32_t water_z, bool on_entity,
                         int32_t surface_type, int foot) {
	if (water_z != 0 && feet_z < water_z) return kSlotFootWater;
	if (on_entity) return foot == 0 ? kSlotFootLObject : kSlotFootRObject;
	if (surface_type == 3) return foot == 0 ? kSlotFootLSnow : kSlotFootRSnow;
	return foot == 0 ? kSlotFootLGround : kSlotFootRGround;
}

// Resolve one sound-profile SLOT to its authored set name for a body identified
// only by its items.def type id — the wire body channel's equivalent of the
// authority path's bound AiProfile index. Mirrors the witnessed fallback chain
// [orig: Entity_GetProfileSlotSound @0x528300 — the female byte @0x52831c; the
//  unresolved-binding fallback to "default", itself falling back to the first
//  profile: ItemDef_AllocateWithDefaults @0x49e3f5 seeds FindSlotByName
//  ("default"), and the find-miss returns the base @0x526e30].
// Returns nullptr when the slot resolves to nothing — the id-0 no-op retail
// treats as "play no sound" rather than a fallback.
inline const std::string *organic_slot_set(const SoundProfileTable &profiles,
                                           const OrganicSoundProfileTable &bindings,
                                           int32_t item_id, bool female, int slot) {
	if (slot < 0 || slot >= kSoundProfileSlotCount) return nullptr;
	if (profiles.entries().empty()) return nullptr;
	// The female byte selects the female binding UNCONDITIONALLY — an
	// unresolved female slot falls through the "default" chain below, exactly
	// like the authority path (AiSystem::emit_slot_sound), never back to the
	// primary. (The def parser seeds both names, so in practice both resolve.)
	int16_t index = -1;
	if (const OrganicSoundProfile *b = bindings.get(item_id))
		index = female ? b->female : b->primary;
	const SoundProfile *p =
			(index >= 0 && static_cast<size_t>(index) < profiles.entries().size())
					? &profiles.entries()[static_cast<size_t>(index)]
					: profiles.find("default");
	if (p == nullptr) return nullptr;
	const std::string &set = p->set_names[static_cast<size_t>(slot)];
	return set.empty() ? nullptr : &set;   // the resolved-id-0 no-op
}

} // namespace opennova::audio
