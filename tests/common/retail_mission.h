// The retail missions the OPENNOVA_JO_DIR tests promote, served the way the
// game serves them: through the install's archive mount. Retail keeps its
// missions inside the .pff set (localres for the base game, <exp>.pff for an
// expansion), so a loose <install>/<name>.bms is never how a stock install
// carries one; the packed reference install CI mounts has none either.
#ifndef OPENNOVA_TEST_RETAIL_MISSION_H
#define OPENNOVA_TEST_RETAIL_MISSION_H

#include "retail_paths.h"

#include <base/resource_index/resource_index.h>

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace retail {

// Read mission `name`: the base mount first, then each expansion the install
// carries, and failing both, loose from the extracted asset tree
// (OPENNOVA_JO_ASSETS carries the shipped .bms at its root). `index` is left
// scanned on the mount that served the mission (the base mount for a loose
// hit) so the caller's asset reads see the same layer; `served_by` names it.
// False when no layer carries the mission.
inline bool read_mission(const std::string &install, const std::string &name,
                         opennova::ResourceIndex &index, std::vector<uint8_t> &out,
                         std::string &served_by) {
	if (index.scan(install) && index.has_file(name) && index.read_file(name, out)) {
		served_by = "the base mount";
		return true;
	}
	for (const std::string &expansion : expansions()) {
		if (index.scan(install, expansion) && index.has_file(name) &&
				index.read_file(name, out)) {
			served_by = "the " + expansion + " mount";
			return true;
		}
	}
	const std::string tree = assets();
	if (!tree.empty()) {
		std::ifstream f(join(tree, name), std::ios::binary);
		if (f) {
			out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
			served_by = "OPENNOVA_JO_ASSETS";
			index.scan(install);
			return !out.empty();
		}
	}
	return false;
}

} // namespace retail

#endif
