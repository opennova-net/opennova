// S13 (ADR 0028): the envs-marker resolution — see envs_markers.h for the
// dispatch witnesses (the faithful STRATEGY_ITEM_SOUNDLOOP path).
#include <runtime/audio/envs_markers.h>

#include <base/io/strutil.h>
#include <formats/mission/mission.h> // kItemIdOffset (BMS type id -> items.def id)

#include <cstring>
#include <unordered_map>

using namespace opennova::def;

namespace opennova::audio {

namespace {

const DefItemDef *item_by_id(const DefItemsFile &items, int32_t item_id) {
	for (size_t i = 0; i < items.count; ++i) {
		if (items.entries[i].id == item_id) return &items.entries[i];
	}
	return nullptr;
}

bool tag_is_envs(const char *tag) {
	return strutil::iequals(tag, "envs");
}

} // namespace

bool item_is_envs(const DefItemsFile &items, int32_t item_id) {
	const DefItemDef *def = item_by_id(items, item_id);
	if (def == nullptr) return false;
	return tag_is_envs(def->ai_function) || tag_is_envs(def->move_function);
}

std::vector<EnvsMarker> resolve_envs_markers(
		const bms::File &mission, const DefItemsFile &items) {
	std::vector<EnvsMarker> out;
	// One id -> def index over the whole walk (missions repeat item ids).
	std::unordered_map<int32_t, const DefItemDef *> by_id;
	by_id.reserve(items.count);
	for (size_t i = 0; i < items.count; ++i)
		by_id.emplace(items.entries[i].id, &items.entries[i]);

	const std::vector<bms::Entity> *groups[] = {
			&mission.markers, &mission.items, &mission.buildings,
			&mission.organics};
	for (const std::vector<bms::Entity> *group : groups) {
		for (const bms::Entity &entity : *group) {
			// The record layer's identity: BMS type id + the items.def offset.
			const auto found =
					by_id.find(entity.type_id + mission::kItemIdOffset);
			if (found == by_id.end()) continue;
			const DefItemDef &def = *found->second;
			if (!tag_is_envs(def.ai_function) && !tag_is_envs(def.move_function))
				continue;
			EnvsMarker marker;
			marker.x = entity.get_x();
			marker.y = entity.get_y();
			marker.z = entity.get_z();
			marker.bms_id = entity.id;
			// soundloop_1..4 select by time-of-day region morning/day/evening/
			// night; slots 5..7 have no region consumer in this updater.
			// [orig: Entity_UpdateEnvSoundEmitter @ 0x4a8080]
			for (int slot = 0; slot < 4; ++slot)
				marker.slot_sets[static_cast<size_t>(slot)] =
						def.soundloops[slot];
			out.push_back(std::move(marker));
		}
	}
	return out;
}

} // namespace opennova::audio
