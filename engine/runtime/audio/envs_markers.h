#pragma once

#include <formats/def/def.h>
#include <formats/mission/bms.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// S13 (ADR 0028): the ambient env-sound marker resolution — which placed
// entities are envs-class emitters and which LWF set each time-of-day slot
// names. The env sound updater is selected by the item's DISPATCH TAG, not by
// the BMS record pool: most authored `snd:` entries are marker records, but
// JOX also places envs decorations in the building pool (oil pumps/flares).
// [orig: the `envs` entry in the class dispatch table @ 0x82ABD4 routes to
//  Entity_UpdateEnvSoundEmitter @ 0x4a8080, which indexes
//  itemDef.soundLoopId[region]; the soundloop_1..7 parse
//  ItemDef_ParseProperty @ 0x49fec4]
namespace opennova::audio {

struct EnvsMarker {
	// Authored BMS mission position (bms::Entity::get_*() units).
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
	int32_t bms_id = 0;
	// soundloop_1..4 = morning/day/evening/night, as authored. An empty slot
	// is SILENT in that region, like the original's null soundLoopId — there
	// is NO fallback (a sound_profile fallback once carried here was
	// unwitnessed and never fires with JO data). The embedder applies its own
	// bank-presence filtering when it materializes candidates.
	std::array<std::string, 4> slot_sets;
};

// The envs dispatch predicate: ai_function or move_function == "envs",
// compared case-insensitively. Unknown ids are not envs.
bool item_is_envs(const opennova::def::DefItemsFile &items, int32_t item_id);

// Every envs-class placed entity across all pools, in the canonical entity
// walk order (markers, items, buildings, organics), with its four authored
// time-of-day slot set names.
std::vector<EnvsMarker> resolve_envs_markers(
		const bms::File &mission, const opennova::def::DefItemsFile &items);

} // namespace opennova::audio
