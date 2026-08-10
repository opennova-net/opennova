// Mission placement policy — the witnessed eligibility predicates deciding
// how a placed entity renders, ported from mission_object_placer.gd
// (2026-08-10 de-scripting). Pure bit policy over the items.def attribs
// (engine/formats/def carries the attrib bit constants) and the mission
// entity record; the shell applier owns every node/mesh build.
#pragma once

#include <cstdint>

namespace opennova::mission {

// items.def type ids consumed by placement (itemdef-re.md +0x5c).
inline constexpr int kItemTypeVehicle = 1;
inline constexpr int kItemTypePerson = 3;
inline constexpr int kItemTypeBuilding = 5;

// Mission entity kinds (the BMS record families).
inline constexpr int kEntityKindMarker = 0;
inline constexpr int kEntityKindItem = 1;
inline constexpr int kEntityKindBuilding = 2;
inline constexpr int kEntityKindOrganic = 3;

// Attrib bits (DEF_ITEM_ATTRIB_NOSHADOW mirrors engine/formats/def).
inline constexpr uint32_t kItemAttribNoShadow = 0x04000000u;
inline constexpr uint32_t kItemAttrib2DynamicShadow = 0x10u;
inline constexpr uint32_t kItemAttrib2StaticShadow = 0x20u;
inline constexpr uint32_t kEntityAttribNoShadow = 0x01000000u;

// Runtime-only player type id -> the authored items.def visual item. The
// runtime entity item/type id stays unchanged; this only chooses
// graphics/ADM data for presentation.
inline constexpr int kPlayerRuntimeTypeId = 0x14B9;
inline constexpr int kPlayerVisualItemId = 105310;

// Live entity silhouettes ride the dynamic projection list: persons always,
// otherwise the item's attrib2 dynamic-shadow bit
// (docs/render/render-lighting-re.md: dynamic slots via Entity_InitFromModel).
inline bool item_casts_dynamic_shadow(int item_type, uint32_t attrib2) {
	return item_type == kItemTypePerson ||
			(attrib2 & kItemAttrib2DynamicShadow) != 0;
}

// Static terrain-tile silhouettes: buildings unless opted out, pool-1 items
// only with the attrib2 StaticShadow bit; the entity-level and item-level
// NoShadow bits both veto (docs/render/render-lighting-re.md: the static
// collector admits pool-2 unless NoShadow and pool-1 only with attrib2
// StaticShadow).
inline bool item_casts_static_terrain_shadow(int entity_kind,
		uint32_t entity_attrib, uint32_t item_attrib, uint32_t item_attrib2) {
	if ((entity_attrib & kEntityAttribNoShadow) != 0 ||
			(item_attrib & kItemAttribNoShadow) != 0) {
		return false;
	}
	return entity_kind == kEntityKindBuilding ||
			(entity_kind == kEntityKindItem &&
					(item_attrib2 & kItemAttrib2StaticShadow) != 0);
}

// Witnessed water-mirror eligibility (env #30): the reflection collects only
// ItemDefType==vehicle entities above water [orig: Entity_InitFromModel
// @ 0x40e20a sets entity+36 flag 0x400 iff ItemDefType(+0x5C)==1;
// Terrain_CollectVisibleEntitiesForReflection @ 0x5c90a0 filters on it].
inline bool item_is_mirror_reflected(int item_type) {
	return item_type == kItemTypeVehicle;
}


// --- BMS <-> presentation-space transforms ---------------------------------
// The mission record is Z-up; the presentation world is Y-up. Position is a
// -90 deg rotation about X; orientation is the structural port of the
// engine's matrix builder conjugated into the Y-up basis
// [orig: Entity_SpawnFromBMSRecord @ 0x40eb66 +
//  Math_BuildFixedPointMatrixFromEulerAngles @ 0x613f40, via
//  Entity_UpdateOrientationMatrix @ 0x43b440]: the engine builds
//  Rz(90-yaw) * Ry(-pitch) * Rx(roll) in its Z-up world; conjugating by the
//  position map (x,y,z)->(x,z,-y) gives
//  R = RotY(90-yaw) * RotZ(pitch) * RotX(roll), and the .3di model-forward
//  correction appends RotY(90). The shell binding only converts vector
//  types.

struct PlacementVec3 {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
};

// Column-vector basis (columns are the images of the unit axes).
struct PlacementBasis {
	PlacementVec3 x;
	PlacementVec3 y;
	PlacementVec3 z;
};

inline PlacementVec3 bms_to_presentation_position(const PlacementVec3 &p) {
	// A -90 deg rotation about X maps (x, y, z) -> (x, z, -y).
	return PlacementVec3{p.x, p.z, -p.y};
}

inline PlacementVec3 presentation_to_bms_position(const PlacementVec3 &p) {
	return PlacementVec3{p.x, -p.z, p.y};
}

PlacementBasis bms_to_presentation_basis(float pitch_deg, float yaw_deg,
		float roll_deg);


// --- placement-pass witness ledger -----------------------------------------
// Retail placement/present behaviors the shell's placer device leg mirrors.
// The addresses live HERE so binding comments can reference them by name
// (witnessed engine behavior is cited in engine/, ADR 0031):
//  - per-model loading-screen presents while placing
//    [orig: Game_StartMission's in-loop LoadingScreen_UpdateAndPresent
//    @ 0x524d9c/0x524f32]
//  - the Ground userpoint is baked into the stored entity position at
//    author-time (place / terrain-drag), never applied at render
//    [orig: sub_401A90, dfx2med.exe]
//  - the first-person arms and gun both draw with the equipped GUN's bone
//    matrices (one shared rig table)
//    [orig: Player_RenderFirstPersonViewModel @ 0x4ded60]
//  - portal buildings render per-section from per-instance visibility masks,
//    so they cannot join a pooled static batch
//    [orig: g_BuildingSectionVisMask consumption + Terrain_RenderSectorModels
//    @ 0x5c5d30; collect_render_objects_for_batch @ 0x5d9156..0x5d9170;
//    docs/render/render-occlusion-re.md]
//  - the MODEL is the rig source for bone matrices, never the lossy .bad
//    records [orig: BoneAnim_BuildWorldMatrices @ 0x40c400 walks modelDef+56]
//  - every entity is relit from the current lighting block each frame
//    [orig: setup_entity_lighting_and_shader_constants @ 0x5d98a0]

} // namespace opennova::mission
