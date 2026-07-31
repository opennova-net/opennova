#ifndef OPENNOVA_MISSION_AUTHORING_H
#define OPENNOVA_MISSION_AUTHORING_H

#include <cstddef>
#include <vector>

#include "mission/mission.h"

// Mission AUTHORING facade: the editing policies the original mission editor
// applies when the user places or moves an entity, expressed over the
// MissionDocument mutation API so they are a reusable engine capability (an
// in-game editor, headless tools, ctest) instead of editor-shell code.
//
// The geometry here is mission (BMS) space: Z-up, the same axes the .bms
// records store. Hosts that render in another space (the Godot editor is
// Y-up) convert positions/anchors with their own linear axis map before
// calling in; the policies below never see embedder coordinates.

namespace opennova::mission::authoring {

// items.def item type -> the BMS entity list a new placement lands in.
// Empirically 1:1 and deterministic across 185k entities in 114 shipping JO
// missions (types per libs/def DefItemType — the witnessed engine values
// [orig: ItemDef_ParseProperty @ 0x49eb00]: 0=unset, 1=vehicle,
// 2=decoration/foliage, 3=person, 4=marker, 5=building, 6=powerup/object,
// 8=effect; 7 unused):
//   Person                          -> Organic
//   Building / Decoration / Foliage -> Building (all three share the list)
//   Marker                          -> Marker   (mesh-less)
//   Vehicle / Object / Powerup / Unknown -> Item
// Decoration and Foliage landing in the Building list (not Item) is the
// non-obvious part and the dominant case in real data.
EntityKind entity_kind_for_item_type(int def_item_type);

// The canonical waypoint marker item id: the engine's "waypoint" type
// (BMS type_id 6005 = items.def id 106005). A saved record with this id is a
// real waypoint the engine follows whether or not the loaded db carries a
// model for it.
constexpr int kWaypointMarkerItemId = 106005;

// The item id for a NEW marker added to `path_index`: reuse the id of the
// path's OWN first marker (a path authored with a specific waypoint variant
// stays consistent and shipped data round-trips with its own id), falling
// back to the canonical id for an empty path. Looks only at the path's own
// members — never the whole scene — so a player start / spawn / sound placed
// elsewhere can never bleed into a waypoint.
int marker_item_id_for_path(const MissionDocument &doc, size_t path_index);

// Author-time Ground-userpoint bake, in mission space: the stored origin is
// the ground hit minus the entity's ROTATED model-local ground anchor, so the
// model's ground point lands exactly on the hit. [orig: sub_401A90,
// dfx2med.exe] The rotation matrix is the engine's own euler builder
// Rz(90-yaw) * Ry(-pitch) * Rx(roll) [orig:
// Math_BuildFixedPointMatrixFromEulerAngles @ 0x613F40 (Jointops.exe)]
// composed with the .3di import's constant model-forward correction
// (RotZ(90) in mission axes — the model's +Z nose onto the engine's +X
// canonical heading), i.e. exactly the conjugate of the Godot-side render
// basis (MissionObjectPlacer.bms_to_godot_basis). `ground_hit_with_rotation`
// carries the hit in x/y/z and the entity rotation in pitch/yaw/roll
// (degrees); the result keeps that rotation with the baked origin.
EntityTransform bake_ground_transform(const EntityTransform &ground_hit_with_rotation,
                                      const float ground_anchor_bms[3]);

// Place a new entity with its ground point at `ground_hit_bms`. The list is
// derived from `def_item_type`; new records are rotation-zero, and at zero
// rotation the editor's bake is the UNROTATED anchor subtraction (markers,
// which are mesh-less, ignore the anchor entirely). Returns false (with the
// document untouched) when the add is rejected.
bool place_entity_grounded(MissionDocument &doc,
                           int item_id,
                           int def_item_type,
                           const float ground_hit_bms[3],
                           const float ground_anchor_bms[3],
                           EntityRecord *out = nullptr);

// Re-ground an existing entity at `ground_hit_bms`, keeping its rotation:
// the full rotated bake (see bake_ground_transform). Markers ignore the
// anchor. Returns false when the entity does not exist or the write is
// rejected.
bool move_entity_grounded(MissionDocument &doc,
                          EntityKind kind,
                          size_t index,
                          const float ground_hit_bms[3],
                          const float ground_anchor_bms[3]);

// Append (or insert at `insert_index`) a new marker on `path_index` at
// `ground_hit_bms`, with the path-consistent item id policy above. Markers
// store the hit directly (no anchor). Thin composition of
// marker_item_id_for_path + MissionDocument::add_waypoint_marker.
bool add_path_marker_grounded(MissionDocument &doc,
                              size_t path_index,
                              const float ground_hit_bms[3],
                              int insert_index = -1,
                              EntityRecord *out_marker = nullptr,
                              WaypointPath *out_path = nullptr);

// One row of a bulk re-ground: the entity, its NEW ground hit (mission space,
// from the embedder's height sampling), and its model-local ground anchor.
struct RegroundRequest {
	EntityKind kind = EntityKind::Item;
	size_t index = 0;
	float ground_hit_bms[3] = {0.0f, 0.0f, 0.0f};
	float ground_anchor_bms[3] = {0.0f, 0.0f, 0.0f};
};

// Bulk re-ground after a terrain height change: for each request, bake the
// grounded origin (rotation kept; markers store the hit directly — the same
// policy as move_entity_grounded) and move the entity when the baked origin
// deviates from the stored one by more than `epsilon` on any axis.
// Within-epsilon rows are skipped, so a no-op terrain edit moves nothing and
// an entity already on the new ground does not churn the document. With
// apply = false nothing is written and the return value is the would-move
// count — the embedder's "terrain changed under N objects" prompt and the apply
// share one policy, so the count can never lie. Returns the number of
// entities moved (or that would move). `out_moved_rows`, when non-null,
// receives the indices into `requests` of the moved (or would-move) rows in
// request order, so an embedder can update its placed world in place instead of
// rebuilding it.
size_t reground_entities(MissionDocument &doc,
                         const RegroundRequest *requests,
                         size_t count,
                         float epsilon = 0.01f,
                         bool apply = true,
                         std::vector<size_t> *out_moved_rows = nullptr);

} // namespace opennova::mission::authoring

#endif // OPENNOVA_MISSION_AUTHORING_H
