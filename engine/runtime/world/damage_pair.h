#pragma once

// The friendly-fire gate every damage pass asks before it lands a hit on a
// victim from a round's owner [orig: Projectile_DamagePairEligible @0x4E74F0].
// Its callers pass (victim, shooter): the projectile damage pass
// (Projectile_ProcessDamageOnTarget @0x4E8085) and the explosion queue's three
// pool legs (@0x4EB086 / @0x4EB45B / @0x4EB7F6). TRUE means the pair is
// PROTECTED: the damage pass jumps straight to its return (@0x4E808F), past the
// shot relations, the health write, the hit record, the class callback and the
// local player's damage feedback; a blast leg passes the victim over (pool 0
// admits it anyway for a medic entry or when the victim is the attacker itself).

#include <runtime/world/world.h>

namespace opennova::world {

// AiSlot[1] & 0x200, the BERSERK behavior bit, read from the entity's AI slot
// when it has one [orig: entity+0x68 -> slot+4 @0x4e752c..0x4e754e].
inline bool damage_pair_berserk(const World &world, const Entity &entity) {
	const AiEntity *ai = world.ai.for_handle(entity.handle);
	return ai != nullptr && (ai->slot.f[AiSlot::kBehaviorFlags] & 0x200) != 0;
}

// The pure rule over the fields the original reads.
//   no_friendly_fire  g_RulesFlags & 0x200 (the host's NoFriendlyFire attribute)
//   shooter_is_player the shooter's Flags & 0x100 (a human player's body)
//   victim_device     the victim's placed-device class when it is a typed item
//                     (ItemTypeIndex != 0) with an ItemDef whose attrib carries
//                     neither EWEAP (0x20) nor PLAYERCONTROL (0x40); kNone else
// [orig: @0x4e74fc..0x4e7599: the rule / player test @0x4e7516, the team bytes
//  @0x4e7528, the two BERSERK reads @0x4e753a / @0x4e754c, the device leg
//  @0x4e7550..0x4e7597 against g_AmmoSatchel / g_AmmoClaymore / g_AmmoAVMine]
inline bool damage_pair_protected_rule(bool no_friendly_fire, bool shooter_is_player,
		uint8_t victim_team, uint8_t shooter_team, bool victim_berserk, bool shooter_berserk,
		ThrowClass victim_device) {
	bool protected_pair = no_friendly_fire || !shooter_is_player;
	if (victim_team != shooter_team) protected_pair = false;
	if (victim_berserk) protected_pair = false;
	if (shooter_berserk) protected_pair = false;
	switch (victim_device) {
	case ThrowClass::kSatchel:
	case ThrowClass::kClaymore:
		protected_pair = false;
		break;
	case ThrowClass::kAVMine:
		return false;
	default:
		break;
	}
	return protected_pair;
}

// The gate over live entities. A missing shooter protects nothing
// [orig: `test esi, esi` @0x4e74f3].
inline bool damage_pair_protected(World &world, const Entity &victim, const Entity *shooter) {
	if (shooter == nullptr) return false;
	ThrowClass device = ThrowClass::kNone;
	if (victim.item_type_index != 0 && victim.has_item_def &&
			(victim.item_attrib & (kItemAttribEweap | kItemAttribPlayerControl)) == 0) {
		if (const PlacedDevice *placed = world.throwables.device_for(victim)) device = placed->think;
	}
	// The Player bit lives on either view of the entity's Flags word (the wire
	// view and the engine view, which the spawns and the readers keep coherent).
	const bool shooter_is_player = ((shooter->flags | shooter->engine_flags) & kEntityFlagPlayer) != 0;
	return damage_pair_protected_rule(world.rules.no_friendly_fire,
			shooter_is_player, victim.team, shooter->team,
			damage_pair_berserk(world, victim), damage_pair_berserk(world, *shooter), device);
}

} // namespace opennova::world
