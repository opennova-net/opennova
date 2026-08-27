// Retail CPT sweep: every .cpt directly under the extracted asset tree parses
// through the CDEP/DPTH/POLY reader into the 1024x1024 depth atlas with at
// least one tile, and the Dvxi5 bake carries the header facts the reader's
// contract was grilled on (terrain_name "Dvxi5", creator "Brophy", CDEP).
// Reports Skipped without OPENNOVA_JO_ASSETS.

#include <formats/cpt/cpt.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include "common/retail_paths.h"

namespace {

bool is_cpt(const std::filesystem::path &path) {
	std::string ext = path.extension().string();
	for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return ext == ".cpt";
}

} // namespace

int main() {
	const std::string assets = retail::assets();
	if (assets.empty())
		return retail::skip("OPENNOVA_JO_ASSETS (the extracted retail tree with its loose .cpt polydata)");
	int failures = 0;
	int swept = 0;
	bool saw_dvxi5 = false;
	for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(assets)) {
		if (!entry.is_regular_file() || !is_cpt(entry.path())) continue;
		const std::string name = entry.path().filename().string();
		opennova::CptFile cpt;
		try {
			cpt = opennova::CptFile::read(entry.path().string());
		} catch (const std::exception &e) {
			std::printf("FAIL: %s: %s\n", name.c_str(), e.what());
			++failures;
			continue;
		}
		++swept;
		if (cpt.depth_buffer.size() != 1024u * 1024u || cpt.tiles.empty()) {
			std::printf("FAIL: %s: depth %zu samples, %zu tiles\n", name.c_str(),
			            cpt.depth_buffer.size(), cpt.tiles.size());
			++failures;
		}
		std::string lower = name;
		for (char &c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		if (lower == "dvxi5.cpt") {
			saw_dvxi5 = true;
			if (std::string(cpt.header.terrain_name) != "Dvxi5" ||
			    std::string(cpt.header.creator) != "Brophy" ||
			    cpt.depth_format != opennova::DepthFormat::CDEP) {
				std::printf("FAIL: Dvxi5.cpt header: name=%s creator=%s cdep=%d\n",
				            cpt.header.terrain_name, cpt.header.creator,
				            cpt.depth_format == opennova::DepthFormat::CDEP);
				++failures;
			}
		}
	}
	if (swept == 0) {
		std::printf("FAIL: no .cpt directly under %s\n", assets.c_str());
		return 1;
	}
	if (!saw_dvxi5) {
		std::printf("FAIL: the extract carries no Dvxi5.cpt\n");
		++failures;
	}
	std::printf("cpt_jo_assets_sweep: %d polydata files, %d failures\n", swept, failures);
	return failures == 0 ? 0 : 1;
}
