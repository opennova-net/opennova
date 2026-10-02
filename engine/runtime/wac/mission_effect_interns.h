#pragma once

// The effect pool's mission-start intern order. Retail resets the pool in
// CEffectSystem_Init and then, before the WAC compile, interns in this order:
// the 78-name material table, ammo.def, weapon.def, the vehicle fire set and
// powerup.def. A script's FX literal therefore compiles to its 1-based slot
// after every name those loads pooled first, and that slot is the operand a
// host replicates in S2C 0x23. Seeding the catalog the same way makes our
// compiled handles retail's, so a retail host's Fx operand names the same
// effect on our joiner, and ours on a retail one.
// [orig: Game_StartMission — CEffectSystem_Init @0x524980 (the pool count
//  zeroed @0x5f614b), CEffectManager_RegisterAllMaterials @0x52499e,
//  AmmoDef_LoadAll("ammo.def") @0x525495, WeaponDefs_LoadFile("weapon.def")
//  @0x5254bd, VehicleEffect_InitAll @0x52563d, PowerUpDef_LoadFromFile
//  ("powerup.def") @0x5256d2, WacScript_InitAndLoad @0x525cb3]

#include <string_view>

namespace opennova::particle { class EffectCatalogNames; }

namespace opennova::wac {

// The three def files' texts as mounted (an absent file is empty).
struct MissionEffectDefTexts {
	std::string_view ammo_def;
	std::string_view weapon_def;
	std::string_view powerup_def;
};

// Interns every name the mission-start loads pool, in their order, into
// `effects` (which already holds the mounted effect documents). A name the
// pool holds keeps its slot; an unknown one clones stockeffect, or is not
// pooled without it [orig: CEffectWorld_InternEffectHandle @0x5F7310].
void intern_mission_start_effects(particle::EffectCatalogNames &effects,
		const MissionEffectDefTexts &defs);

} // namespace opennova::wac
