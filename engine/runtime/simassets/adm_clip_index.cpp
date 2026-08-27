#include <runtime/simassets/adm_clip_index.h>

#include <formats/adm/adm.h>
#include <formats/bad/bad.h>
#include <base/io/strutil.h>
#include <base/resource_index/resource_index.h>

namespace opennova::simassets {

namespace {

std::string resolve_bad(const std::string &value) {
	if (!strutil::ends_with_icase(value, ".bad")) {
		return value + ".bad";
	}
	return value;
}

} // namespace

void AdmClipIndex::clear() {
	adm_name_.clear();
	lengths_.clear();
}

int AdmClipIndex::load(const opennova::ResourceIndex *index,
                       const std::string &adm_name) {
	clear();
	if (index == nullptr || adm_name.empty()) {
		return 0;
	}
	std::string name = adm_name;
	if (!strutil::ends_with_icase(name, ".adm")) {
		name += ".adm";
	}
	std::vector<uint8_t> adm_bytes;
	if (!index->read_file(name, adm_bytes) || adm_bytes.empty()) {
		return 0;
	}
	AdmFile adm;
	if (adm_parse_buffer(reinterpret_cast<const char *>(adm_bytes.data()),
	                     adm_bytes.size(), &adm) != 0) {
		return 0;
	}
	adm_name_ = name;

	for (size_t i = 0; i < adm.count; ++i) {
		const std::string key = strutil::to_lower(adm.entries[i].key);
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
			std::vector<uint8_t> bad_bytes;
			BadFile bf;
			if (!index->read_file(resolve_bad(value), bad_bytes) ||
					bad_bytes.empty() ||
					bad_parse_buffer(bad_bytes.data(), bad_bytes.size(), &bf) != 0) {
				continue; // continue-on-failure: the registration behavior
			}
			lengths.push_back(bf.fps > 0 && bf.frame_count > 0
					? static_cast<float>(bf.frame_count) / static_cast<float>(bf.fps)
					: 0.0f);
			bad_free(&bf);
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

} // namespace opennova::simassets
