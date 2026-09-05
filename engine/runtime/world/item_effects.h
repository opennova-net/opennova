// The per-item ITEMS.DEF particle-effect law — slot A ('particlefx <effect>
// <userpoint>'), the always-on emitter the mission-start walk attaches to a
// placed entity: which pools/attribs admit the attach, which identity aliases
// a controller lifecycle event names, and where on the model the emitters go.
// Portable decisions over the parsed item row's attrib word (formats/def) and
// the model's userpoint table (formats/threedi); the presenting shell
// (godot/src/world/item_effect_director) spawns the groups and follows the
// nodes. The watercraft W3/W4 lanes route separately through the portable
// vehicle motor and fixed-tick presenter; fxs/W1/W2 and the death/fire/other
// family remain movement/damage-state threads (ptl-format-re.md §8).
// [orig: resolve_item_materials_and_spawn_bone_trails @ 0x522ee0 (mission
//  start, entity pools 1-3) -> ItemDef_GetBoneMaskByName @ 0x49ea40 (first
//  16, stricmp) -> Entity_SpawnBoneTrailEffect @ 0x43bef0 (one mode-2 attached
//  emitter per masked userpoint: pos = the userpoint, forward = its
//  direction)]
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <formats/threedi/threedi_3di3.h>

namespace opennova::world {

// The original walks entity pools 1-3 with distinct attrib masks; `kind` is
// the imported mission kind (EntityKind: Marker=0, Item=1, Building=2,
// Organic=3, any other value admits nothing). Pool 0 organics are excluded;
// pool 1 items skip attrib 0x42 (POWERUP | PLAYERCONTROL); pools 2/3 —
// buildings and markers — skip only the powerup bit 0x2.
// [orig: resolve_item_materials_and_spawn_bone_trails @ 0x522ee0 — the
//  three per-pool attrib gates of its pool walk]
bool item_effect_pool_allows(int kind, uint32_t attrib);

// The occupied-controller pass bypasses only PlayerControl: an Item whose
// attrib carries PLAYERCONTROL and not POWERUP attaches once a controlling
// occupant arrives (the vehicle_control_* lifecycle edges) and detaches when
// the last one leaves. The independent powerup exclusion remains intact.
bool item_effect_controller_allows(int kind, uint32_t attrib);

// The identity aliases one entity answers to across the seams a controller
// lifecycle event may name it by: "wire:<handle>" (a 16-bit wire handle;
// 0xffff = none), "net:<id>" (a positive simulation net id), "bms:<id>" (a
// positive BMS record id) and "origin:<packed>" (a positive spawn-origin
// word, entity.h spawn_origin_pack). Synthetic items.def attachments all
// carry the same sentinel origin (-1 or kSpawnOriginNone) and no authored
// BMS/net identity: their packed runtime handle is therefore the ONLY alias
// that distinguishes siblings on the same carrier, so a wire-identified
// entity with bms 0 and the sentinel origin answers to the wire alias alone.
void item_effect_identity_aliases(int32_t net_id, int32_t bms_id,
		int64_t spawn_origin, int32_t wire_handle,
		std::vector<std::string> &out);

// True when the two alias lists share a member (the event <-> node match).
bool item_effect_aliases_intersect(const std::vector<std::string> &left,
		const std::vector<std::string> &right);

// The attach plan for one model: every userpoint the authored name masks in
// the model's FIRST 16 (exact case-insensitive match, duplicate names all
// match — the 16-bit mask of the original), else the spawn_count==0 leg —
// ONE emitter at the entity origin (no matched point, or no authored point
// name). The shell composes each userpoint's position + direction with the
// entity pose; the origin leg keeps the entity basis.
// [orig: ItemDef_GetBoneMaskByName @ 0x49ea40 (formats/threedi
//  threedi_3di3_user_point_mask carries the scan); the spawn_count==0 leg
//  Entity_SpawnBoneTrailEffect @ 0x43c097 -> submit_effect_descriptor
//  @ 0x43c0a4 at entity->Position]
struct ItemEffectAttachPlan {
	uint16_t mask = 0;             // the first-16 match mask
	std::vector<int> user_points;  // one attach per set bit, ascending
	bool origin_fallback = false;  // no match: one emitter at the entity origin
};

ItemEffectAttachPlan item_effect_attach_plan(const opennova::threedi::Threedi3di3 &model,
		const char *userpoint_name);

} // namespace opennova::world
