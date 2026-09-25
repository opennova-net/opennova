#include <runtime/anim/adm_clip_index.h>

#include <formats/adm/adm.h>
#include <formats/bad/bad.h>
#include <base/io/strutil.h>
#include <runtime/assets/asset_store.h>

using namespace opennova::adm;
using namespace opennova::bad;

namespace opennova::anim {

void AdmClipIndex::clear() {
	adm_name_.clear();
	lengths_.clear();
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
	adm_name_ = name;

	for (size_t i = 0; i < adm.count; ++i) {
		// The slot the row's key names past its first five characters, as
		// every lookup spells it (`ANIM_IDLE` and `xxxx_idle` are anim_idle)
		// [orig: AnimMap_FindSlotByName @ 0x40cfa0, stricmp on key + 5].
		const std::string key = adm_slot_key(adm.entries[i].key);
		if (key.empty()) {
			continue;
		}
		// Every quoted token on the row is a VARIANT of the same slot,
		// registered in file order (authored duplication is the rotation
		// weighting) [orig: AnimMap_ParseConfigLine @ 0x40cb60;
		// AnimMap_RegisterBoneNode @ 0x40c2d0].
		const size_t variant_count = adm.entries[i].variant_count;
		std::vector<float> &lengths = lengths_[key];
		for (size_t v = 0; v < variant_count; ++v) {
			const char *value = adm.entries[i].variants[v];
			if (value == nullptr || value[0] == '\0') {
				continue;
			}
			const auto file = assets->bone_animation(value);
			if (!file) continue;
			const BadFile &bf = *file;
			lengths.push_back(bf.fps > 0 && bf.frame_count > 0
					? static_cast<float>(bf.frame_count) / static_cast<float>(bf.fps)
					: 0.0f);
		}
		if (lengths.empty()) {
			lengths_.erase(key);
		}
	}
	return static_cast<int>(lengths_.size());
}

const std::vector<float> *AdmClipIndex::lengths_for(const std::string &key) const {
	auto it = lengths_.find(strutil::to_lower(key));
	return it != lengths_.end() ? &it->second : nullptr;
}

} // namespace opennova::anim
