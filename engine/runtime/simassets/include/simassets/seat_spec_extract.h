// The mission item seat-spec extraction (S4, ADR 0028): build the typed
// ItemSeatSpec table from the retained items.def rows + the sim's own model
// parses — the seat/armory/emplacement userpoint walk with the witnessed name
// prefixes, retail slot layout, yaw-zero local frames, and the phrase_set
// mount config. Replaces the shell's GDScript extraction + Dictionary seam.
// [orig: seat typing Entity_GetBoneSlotType @ 0x434ed0 (strnicmp sitex/ctrlx/
//  UseGun/drvrx); slot layout ItemDef seatBoneIndex/controlBone/useGunBone
//  +0x25D..+0x266; armory gate itemDef->attrib & 0x80000 @ 0x4361ee/@ 0x5a36f5
//  with the "armory" prefix walk @ 0x436226/@ 0x5a372b; addeweap anchors
//  docs/world/itemdef-re.md §child-emplacements]
#ifndef OPENNOVA_SIMASSETS_SEAT_SPEC_EXTRACT_H
#define OPENNOVA_SIMASSETS_SEAT_SPEC_EXTRACT_H

#include <def/def.h>
#include <mission/promote.h>
#include <threedi/threedi_3di3.h>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace opennova::simassets {

struct SeatSpecExtraction {
	// Sorted by type_id (the runtime table's binary-search order).
	std::vector<mission::ItemSeatSpec> specs;
	// The graphic key per emitted type — the mounted-pose model source
	// (SimModelCache::model_for takes exactly this key).
	std::unordered_map<int32_t, std::string> graphic_by_type;
};

// The model source: graphic key -> parsed model (null = unresolvable). The
// embedder wraps its SimModelCache; tests supply in-memory models.
using ModelLookupFn = std::function<const Threedi3di3 *(const std::string &)>;

// Extract specs for the seed items.def ids (full 1xxxxx ids) and,
// transitively, every authored addeweap child. An item lands a spec only when
// it carries runtime metadata (seats, armory points, attachments, a primary
// weapon, or an authored phrase_set). Duplicate/unknown/graphic-less ids and
// unresolvable models degrade exactly like the shell extractor: authored
// attachment rows survive without model anchors; seats/armory need the model.
void extract_item_seat_specs(const DefItemsFile &items,
                             const ModelLookupFn &model_for,
                             const std::vector<int> &seed_item_ids,
                             SeatSpecExtraction &out);

// The userpoint-local conversions, exposed for tests: the authored 16.16
// model point into the mission-local seat frame (the yaw-zero correction
// baked in), and the authored direction into the seat yaw offset in degrees.
world::Vec3 seat_local_from_user_point(const ThreediUserPoint &point);
int seat_yaw_offset_from_user_point(const ThreediUserPoint &point);

} // namespace opennova::simassets

#endif // OPENNOVA_SIMASSETS_SEAT_SPEC_EXTRACT_H
