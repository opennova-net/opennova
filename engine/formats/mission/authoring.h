#ifndef OPENNOVA_MISSION_AUTHORING_H
#define OPENNOVA_MISSION_AUTHORING_H

#include <formats/mission/mission.h>

namespace opennova::mission::authoring {

// items.def item type -> the BMS entity list a new placement lands in.
// Empirically 1:1 and deterministic across 185k entities in 114 shipping JO
// missions (types per engine/formats/def DefItemType — the witnessed engine values
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

} // namespace opennova::mission::authoring

#endif // OPENNOVA_MISSION_AUTHORING_H
