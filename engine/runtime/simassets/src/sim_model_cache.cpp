#include "simassets/sim_model_cache.h"

#include <resource_index/resource_index.h>

#include <cstdlib>
#include <vector>

namespace opennova::simassets {

namespace {

// The render path's name rule (MissionObjectPlacer._model_name_for): the
// graphic's basename + ".3di", matched case-insensitively by the index.
std::string model_name_for(const std::string &graphic) {
	size_t start = graphic.find_last_of("/\\");
	start = (start == std::string::npos) ? 0 : start + 1;
	size_t end = graphic.find_last_of('.');
	if (end == std::string::npos || end < start) end = graphic.size();
	std::string base = graphic.substr(start, end - start);
	for (char &c : base) {
		if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
	}
	return base;
}

} // namespace

SimModelCache::~SimModelCache() {
	reset();
}

void SimModelCache::set_index(const opennova::ResourceIndex *index) {
	if (index_ != index) reset();
	index_ = index;
}

void SimModelCache::reset() {
	for (auto &entry : by_name_) {
		if (entry.second != nullptr) {
			threedi_3di3_free(entry.second);
			std::free(entry.second);
		}
	}
	by_name_.clear();
	parsed_count_ = 0;
	negative_count_ = 0;
}

const Threedi3di3 *SimModelCache::model_for(const std::string &graphic) {
	const std::string base = model_name_for(graphic);
	if (base.empty()) return nullptr;
	auto it = by_name_.find(base);
	if (it != by_name_.end()) return it->second;

	Threedi3di3 *model = nullptr;
	std::vector<uint8_t> bytes;
	if (index_ != nullptr && index_->read_file(base + ".3di", bytes) &&
			!bytes.empty()) {
		auto *parsed = static_cast<Threedi3di3 *>(
				std::calloc(1, sizeof(Threedi3di3)));
		if (parsed != nullptr) {
			if (threedi_3di3_read_memory(bytes.data(), bytes.size(), parsed) == 0) {
				model = parsed;
			} else {
				std::free(parsed);
			}
		}
	}
	if (model != nullptr) {
		++parsed_count_;
	} else {
		++negative_count_;
	}
	by_name_.emplace(base, model);
	return model;
}

} // namespace opennova::simassets
