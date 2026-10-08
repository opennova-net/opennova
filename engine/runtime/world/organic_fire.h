#pragma once

#include <array>
#include <cstdint>

#include <runtime/anim/anim_event_bits.h>

namespace opennova::threedi {
struct Threedi3di3;
}

namespace opennova::world {

struct AmmoTable;

// An NPC body's clip fire: the item's four ammo bytes and three launch points, and the shots one pass of the
// body's fire block takes from an anim event word. The rules the game's org1 think runs (AiSystem's
// infantry_fire_pass) and the editor's clip preview (ADR 0046 DI-24) share, one function each.

// The four ammo bytes (zero none), closeattack, easyrocket, advancedrocket and marker3, and the three one-based
// launch points on the person's own model (zero none), closeattack, rocket (both rocket ammos) and marker3.
// [orig: Entity_InitOrganicAI @0x4BFCC0 -> entity+0x358..+0x35B, +0x365..+0x367]
struct OrganicWeapons {
	std::array<uint8_t, 4> ammo{};
	std::array<uint8_t, 3> launch{};
};
inline constexpr int kOrganicAmmoSlots = 4;
inline constexpr int kOrganicLaunchSlots = 3;
// The items.def fields each slot is read from, in the slots' order.
inline constexpr const char *kOrganicAmmoFields[kOrganicAmmoSlots] = {"ammo_closeattack", "ammo_easyrocket",
                                                                      "ammo_advancedrocket", "ammo_marker3"};
inline constexpr const char *kOrganicLaunchFields[kOrganicLaunchSlots] = {"launchups_closeattack", "launchups_rocket",
                                                                          "launchups_marker3"};

// The definition callback's resolve [orig: Entity_InitOrganicAI @0x4BFCC0]: each ammo name through the ammo
// table's first case-insensitive match [orig: AmmoDef_LookupByName @0x409870, the calls @0x4BFE27..0x4BFE81],
// a miss storing zero and an absent name leaving the byte as it was; each launch name the first
// case-insensitive user point of `model` plus one, the store wrapping to a byte, zero where the model, its
// points or the name are missing [orig: ModelGPM_FindUserpointByName, @0x4BFE8F..0x4BFF82]. `ammo_names` and
// `launch_names` are in the slots' order (null or "" absent).
void resolve_organic_weapons(const char *const ammo_names[kOrganicAmmoSlots],
                             const char *const launch_names[kOrganicLaunchSlots], const AmmoTable &ammo,
                             const threedi::Threedi3di3 *model, OrganicWeapons &out);

// One shot of the fire block: the ammo slot it fires, the launch point it leaves from, the event bit that
// asked for it (0: the walking fire's latch, raised outside the word), and whether it spends a magazine round.
struct OrganicFireShot {
	uint8_t ammo_slot = 0;
	uint8_t launch_slot = 0;
	uint32_t bit = 0;
	bool magazine = false;
};
inline constexpr int kOrganicFireShotMax = 4;
// One pass of the block: its shots in the order it fires them, and whether it aims (it writes aimRef0).
struct OrganicFirePass {
	OrganicFireShot shots[kOrganicFireShotMax];
	int count = 0;
	bool aimed = false;
};

// The shots one pass of an NPC body's fire block takes [orig: Entity_UpdateInfantryAI @0x4BF15C..0x4BF4AD].
// `read_tick` is the body's odd tick (the same gate as its sound block, world::anim_sound_tick, @0x4BF156);
// on it the word's 0x4 fires the closeattack ammo from the closeattack point (a call whatever the byte: a zero
// byte fires nothing) [orig: @0x4BF322..0x4BF35C], 0x8 raises the secondary latch [orig: @0x4BF39B], and 0x10
// fires the marker3 ammo from the marker3 point [orig: @0x4BF3A6..0x4BF3E0]. The latch (raised by the word,
// or already by the walking fire: `latch`) is consumed on every tick, outside the gate [orig: @0x4BF406]: a
// nonzero easyrocket fires from the rocket point and spends a magazine round whether or not a round flies,
// then a nonzero advancedrocket that differs from it fires from the same point at no cost [orig:
// @0x4BF414..0x4BF498]. Nothing tests the magazine or the weapon's state: a clip's fire event always fires.
// A player's body (org2) has no fire block: its twin of the sound block reads no fire bit.
inline OrganicFirePass organic_fire_pass(uint32_t events, bool read_tick, bool latch,
                                         const std::array<uint8_t, 4> &ammo) {
	OrganicFirePass pass;
	const auto add = [&](uint8_t ammo_slot, uint8_t launch_slot, uint32_t bit, bool magazine) {
		OrganicFireShot &shot = pass.shots[pass.count++];
		shot.ammo_slot = ammo_slot;
		shot.launch_slot = launch_slot;
		shot.bit = bit;
		shot.magazine = magazine;
	};
	uint32_t latch_bit = 0;
	if (read_tick) {
		if ((events & anim::kAnimEventFirePrimary) != 0) {
			add(0, 0, anim::kAnimEventFirePrimary, false);
			pass.aimed = true;
		}
		if ((events & anim::kAnimEventFireSecondary) != 0) {
			latch = true;
			latch_bit = anim::kAnimEventFireSecondary;
		}
		if ((events & anim::kAnimEventFireMarker3) != 0) {
			add(3, 2, anim::kAnimEventFireMarker3, false);
			pass.aimed = true;
		}
	}
	if (latch) {
		pass.aimed = true;
		if (ammo[1] != 0) add(1, 1, latch_bit, true);
		if (ammo[2] != 0 && ammo[2] != ammo[1]) add(2, 1, latch_bit, false);
	}
	return pass;
}

} // namespace opennova::world
