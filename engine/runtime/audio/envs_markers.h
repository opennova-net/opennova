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
namespace opennova::world {
class World;
}

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

// The envs dispatch predicate on one items.def entry: ai_function or
// move_function == "envs", compared case-insensitively. The walk below
// applies it to every placed entity whose def it finds.
bool item_is_envs(const opennova::def::DefItemDef &def);

// One entity's envs registration as every walk reads it off its def: false for
// a def that is no envs-class item, else `out` takes its four authored
// time-of-day slot sets, soundloop_1..4 = morning/day/evening/night (slots
// 5..7 have no region consumer in this updater; an empty slot is silent)
// [orig: Entity_UpdateEnvSoundEmitter @ 0x4a8080 indexes soundLoopId[region]].
// The walks below and the editor's Listen (ADR 0046 DI-36) share it.
bool envs_slot_sets(const opennova::def::DefItemDef &def, std::array<std::string, 4> &out);

// The cohort and clock stagger a resolved marker registers with: its place
// among the markers the embedder resolved, its low nibble. Retail seeds both
// from the entity's pool slot (`(handle & 0xF) << 11` on the clock @ 0x408158,
// the pool-2 cohort `tick & 7` @ 0x4c225a); the port's markers carry no pool
// slot, so the walk order stands in for it. MissionAudio and the editor's
// Listen register through it alike.
inline int32_t envs_stagger_slot(size_t resolved_index) {
	return static_cast<int32_t>(resolved_index & 0xFu);
}

// Every envs-class placed entity across all pools, in the canonical entity
// walk order (markers, items, buildings, organics), with its four authored
// time-of-day slot set names.
std::vector<EnvsMarker> resolve_envs_markers(
		const bms::File &mission, const opennova::def::DefItemsFile &items);

// A header-only join's mission document carries no entities: a joiner's envs
// emitters are the entities the host streamed. The same predicate over the
// registry, in the same walk (pools 3 markers, 1 items, 2 buildings, 0
// organics), each in slot order; the position is the mission frame the
// registry keeps, the id the row's placed bms_id.
// [orig: the class dispatch runs Entity_UpdateEnvSoundEmitter @0x4a8080 for
//  every envs entity in the pools, loaded or streamed alike]
std::vector<EnvsMarker> resolve_envs_markers(
		const world::World &world, const opennova::def::DefItemsFile &items);

} // namespace opennova::audio
