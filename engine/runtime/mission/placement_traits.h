// Mission placement policy — the witnessed eligibility predicates deciding
// how a placed entity renders, ported from mission_object_placer.gd
// (2026-08-10 de-scripting). Pure bit policy over the items.def attribs
// (engine/formats/def carries the attrib bit constants) and the mission
// entity record; the shell applier owns every node/mesh build.
#pragma once

#include <cstdint>

#include <formats/mission/mission.h> // kItemIdOffset (formats/mission)

namespace opennova::mission {

// items.def type ids consumed by placement (itemdef-re.md +0x5c).
inline constexpr int kItemTypeVehicle = 1;
inline constexpr int kItemTypePerson = 3;
inline constexpr int kItemTypeBuilding = 5;

// Mission entity kinds (the BMS record families) as the plain ints the
// dictionary/int seams carry; the value authority is EntityKind above.
inline constexpr int kEntityKindMarker = static_cast<int>(EntityKind::Marker);
inline constexpr int kEntityKindItem = static_cast<int>(EntityKind::Item);
inline constexpr int kEntityKindBuilding = static_cast<int>(EntityKind::Building);
inline constexpr int kEntityKindOrganic = static_cast<int>(EntityKind::Organic);

// Attrib bits (DEF_ITEM_ATTRIB_NOSHADOW mirrors engine/formats/def).
inline constexpr uint32_t kItemAttribNoShadow = 0x04000000u;
inline constexpr uint32_t kItemAttrib2DynamicShadow = 0x10u;
inline constexpr uint32_t kItemAttrib2StaticShadow = 0x20u;
inline constexpr uint32_t kEntityAttribNoShadow = 0x01000000u;
// The authored per-record reflect bit (formats/mission
// bms::BmsiAttributeFlags::Reflective).
inline constexpr uint32_t kEntityAttribMirrorReflect = 0x00800000u;

// Runtime-only player type id -> the authored items.def visual item. The
// runtime entity item/type id stays unchanged; this only chooses
// graphics/ADM data for presentation.
inline constexpr int kPlayerRuntimeTypeId = 0x14B9;
// items.def "Player #1, Single player" -> US01/US01.adm.
inline constexpr int kPlayerVisualItemId = 105310;

// The items.def id presentation resolves graphics/ADM by, for a runtime
// entity type id: the runtime-only player type maps to the authored visual
// item when the catalog carries it (`player_visual_in_catalog` — the shell's
// item database probe); every other type is the wire/BMS id + kItemIdOffset.
inline constexpr int visual_item_id_for_runtime_type(
		int item_id, bool player_visual_in_catalog) {
	if (item_id == kPlayerRuntimeTypeId && player_visual_in_catalog)
		return kPlayerVisualItemId;
	return item_id + kItemIdOffset;
}

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

// Witnessed water-mirror eligibility (env #30): every reflected-world leg —
// the sector-model (building) pass AND the entity waves — draws only records
// whose visibility collection passed the above-water filterMask 0x400 against
// entity+36; a below-water view collects unfiltered. Flag 0x400 has exactly
// two writers: vehicles by item type, and the mission-authored per-record
// Reflective attribute (any pool — shipped missions author it on buildings
// and static items). [orig: Terrain_CollectVisibleEntitiesForReflection
// @ 0x5c90a0 (mask = below-water ? 0 : 0x400) -> collect_visible_sector_
// userpoints @ 0x5c6c32..0x5c6c39 + Terrain_CollectVisibleEntities_0
// @ 0x5c6f20 + collect_visible_entities_for_terrain @ 0x5c8c60 pools 0/1;
// consumed by Water_RenderReflectedWorldScene @ 0x5c8510 (sector models
// @ 0x5c8576 -> Terrain_RenderSectorModels @ 0x5c5d30, entity waves
// @ 0x5c857b/0x5c8590/0x5c8599); writers Entity_InitFromModel
// @ 0x40e208..0x40e20a (ItemDefType(+0x5C)==1) and Entity_SpawnFromBMSRecord
// @ 0x40ed1d..0x40ed2b (BMS attrib 0x800000 -> flags 0x400)].
inline bool item_is_mirror_reflected(int item_type) {
	return item_type == kItemTypeVehicle;
}

inline bool placement_is_mirror_reflected(uint32_t entity_attrib,
		int item_type) {
	return item_is_mirror_reflected(item_type) ||
			(entity_attrib & kEntityAttribMirrorReflect) != 0;
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
