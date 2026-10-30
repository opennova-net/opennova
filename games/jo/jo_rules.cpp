#include "jo.h"

namespace opennova::games {

// Joint Operations' rules: Jointops.exe's, the baseline every in-match port was witnessed on, so
// each knob keeps the default runtime/world/title_rules.h gives it, cited there on this binary
// (docs/titles/dfx2-vs-jo-re.md §7, the §8 rows): the linked-spawn-point waypoint hook
// [orig: EventTrigger_MarkLinkedSpawnPoints @0x452ce0], the goals-met advance [orig:
// Player_UpdatePerFrame @0x4de350 (@0x4de68e)], the built-in score table and its fanfare pair
// [orig: GameType_CreateDefaultSettings @0x52DD00], the zone table and the clamp after the
// scales [orig: Weapon_CalcImpactDamage @ 0x4EC920], the occupant scale [orig:
// Entity_ApplyWeaponDamage @ 0x4e6820 (@0x4e69d3)], the 2790 linger and the 620 deploy
// protection [orig: Server_ProcessRoundEnd @0x5164F0 (@0x5166c4); Server_DeployPlayer @0x517740
// (@0x517937, @0x517952)], the medic and the revive window [orig:
// GameEvent_HandleMedicInteraction @0x4e6790; GameEvent_PlayerDeath @0x516DD0 (@0x516f4f)], the
// gun channel [orig: Entity_UpdateTransformAndTurret @0x440ca0; Entity_UpdateChildAttachment
// @0x4409a0], the bone walk [orig: Physics_RaycastAgainstBoneSections @0x4e4670], the AI
// write-off [orig: AI_CheckVehicleStuckState @0x465290] and the spawn kit page [orig: the page at
// 0x833BF8 @ 0x5246be / @ 0x5519e4].
const world::TitleRules &jo_rules() {
	static const world::TitleRules rules;
	return rules;
}

} // namespace opennova::games
