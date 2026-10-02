#include <runtime/anim/adm_clip_index.h>

#include <formats/adm/adm.h>
#include <formats/bad/bad.h>
#include <base/io/strutil.h>
#include <runtime/anim/anim_slot_names.h>
#include <runtime/assets/asset_store.h>

using namespace opennova::adm;
using namespace opennova::bad;

namespace opennova::anim {

int adm_slot_index(std::string_view key) {
	// [orig: AnimMap_FindSlotByName @0x40cfa0 — stricmp of key + 5 over the
	//  252 names of g_AnimStateNameTable @0x8135F0]
	const std::string_view name = adm_slot_name(key);
	if (name.empty()) return -1;
	for (int slot = 0; slot < kAnimSlotCount; ++slot)
		if (strutil::iequals(name, kAnimSlotNames[slot])) return slot;
	return -1;
}

void AdmClipIndex::clear() {
	clips_.clear();
}

int AdmClipIndex::load(const opennova::assets::AssetStore *assets,
                       const std::string &adm_name) {
	clear();
	if (assets == nullptr || adm_name.empty()) {
		return 0;
	}
	std::string name = adm_name;
	if (!strutil::ends_with_icase(name, ".adm")) {
		name += ".adm";
	}
	const auto map = assets->animation_map(name);
	if (!map) return 0;
	const AdmFile &adm = *map;

	for (size_t i = 0; i < adm.count; ++i) {
		// The slot the row's key names past its first five characters, as
		// every lookup spells it (`ANIM_IDLE` and `xxxx_idle` are anim_idle);
		// a key naming none of the 252 slots registers nothing
		// [orig: AnimMap_FindSlotByName @ 0x40cfa0, stricmp on key + 5;
		//  AnimMap_ParseConfigLine's found-slot gate @ 0x40cba4].
		if (adm_slot_index(adm.entries[i].key) < 0) {
			continue;
		}
		const std::string key = adm_slot_key(adm.entries[i].key);
		// Every quoted token on the row is a VARIANT of the same slot,
		// registered in file order (authored duplication is the rotation
		// weighting) [orig: AnimMap_ParseConfigLine @ 0x40cb60;
		// AnimMap_RegisterBoneNode @ 0x40c2d0].
		const size_t variant_count = adm.entries[i].variant_count;
		std::vector<AdmClipFacts> &clips = clips_[key];
		for (size_t v = 0; v < variant_count; ++v) {
			const char *value = adm.entries[i].variants[v];
			if (value == nullptr || value[0] == '\0') {
				continue;
			}
			// A token whose .bad does not load registers failsafe.bad, else nothing.
			const auto file = adm_token_clip(*assets, value);
			if (!file) continue;
			const BadFile &bf = *file;
			AdmClipFacts facts;
			facts.seconds = bf.fps > 0 && bf.frame_count > 0
					? static_cast<float>(bf.frame_count) / static_cast<float>(bf.fps)
					: 0.0f;
			facts.fps = bf.fps;
			facts.frames = bf.frame_count;
			facts.loop = (bf.flags & 0x1u) != 0; // [orig: the loop bit @ 0x40B167]
			clips.push_back(facts);
		}
		if (clips.empty()) clips_.erase(key);
	}
	return static_cast<int>(clips_.size());
}

const std::vector<AdmClipFacts> *AdmClipIndex::clips_for(const std::string &key) const {
	// The query names its slot as a row does [orig: AnimMap_FindSlotByName
	// @ 0x40cfa0, stricmp on key + 5].
	auto it = clips_.find(adm_slot_key(key));
	return it != clips_.end() ? &it->second : nullptr;
}

} // namespace opennova::anim
