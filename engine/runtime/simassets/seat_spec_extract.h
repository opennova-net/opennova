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

#include <formats/def/def.h>
#include <runtime/mission/promote.h>
#include <formats/threedi/threedi_3di3.h>

#include <algorithm>
#include <cstdint>
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
using ModelLookupFn = std::function<const opennova::threedi::Threedi3di3 *(const std::string &)>;

// Extract specs for the seed items.def ids (full 1xxxxx ids) and,
// transitively, every authored addeweap child. An item lands a spec only when
// it carries runtime metadata (seats, armory points, attachments, a primary
// weapon, or an authored phrase_set). Duplicate/unknown/graphic-less ids and
// unresolvable models degrade exactly like the shell extractor: authored
// attachment rows survive without model anchors; seats/armory need the model.
void extract_item_seat_specs(const opennova::def::DefItemsFile &items,
                             const ModelLookupFn &model_for,
                             const std::vector<int> &seed_item_ids,
                             SeatSpecExtraction &out);

// The userpoint-local conversions, exposed for tests: the authored 16.16
// model point into the mission-local seat frame (the yaw-zero correction
// baked in), and the authored direction into the seat yaw offset in degrees.
world::Vec3 seat_local_from_user_point(const opennova::threedi::ThreediUserPoint &point);
int seat_yaw_offset_from_user_point(const opennova::threedi::ThreediUserPoint &point);

// The installed-table lookup (moved from npruntime's joiner bridge — a pure
// specs probe belongs beside the extraction, below the net stack).
inline const mission::ItemSeatSpec *item_seat_spec_for_type(
		const std::vector<mission::ItemSeatSpec> &specs,
		uint16_t type_id) {
	// specs are sorted by type_id at install (finalize/extract); a joiner
	// probes this per present row per frame, so the scan is a binary search.
	const auto it = std::lower_bound(
			specs.begin(), specs.end(), static_cast<int32_t>(type_id),
			[](const mission::ItemSeatSpec &spec, int32_t t) {
				return spec.type_id < t;
			});
	if (it != specs.end() && it->type_id == static_cast<int32_t>(type_id))
		return &*it;
	return nullptr;
}

// Stamp each spec's primary-weapon turret window from the loaded weapon
// table (BAM quartets consumed by the emplaced clamp chain).
void stamp_seat_spec_turret_limits(world::World &world,
		std::vector<mission::ItemSeatSpec> &specs);

// Re-apply the installed table to ONE live entity: emplacement-attachment
// identity (a promoted child on an authority/complete-BMS world preserves
// its authored slot — p_wire_header_world true disables that, matching the
// header-only joiner), the def-derived trait channel, and the seat rows,
// keeping occupants synchronized by retail's fixed mountHandles slot.
void refresh_item_seat_spec(world::World &world,
		const std::vector<mission::ItemSeatSpec> &specs,
		world::Entity &entity, bool p_wire_header_world);

} // namespace opennova::simassets

#endif // OPENNOVA_SIMASSETS_SEAT_SPEC_EXTRACT_H
