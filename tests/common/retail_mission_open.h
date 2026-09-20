// Open a retail mission into the shared rig the way the OPENNOVA_JO_DIR tests
// read missions: through the install's archive mount, the way the game serves
// them. Retail keeps its missions inside the .pff set (localres for the base
// game, <exp>.pff for an expansion), so a loose <install>/<name>.bms is never
// how a stock install carries one; the packed reference install CI mounts has
// none either. The order is the base mount first, then each expansion the
// install carries, then the loose copy under OPENNOVA_JO_ASSETS. The rig's
// own index stays scanned on the layer that served the mission and becomes
// the kernel's asset source, so the kernel boot (seat specs, .adm clips,
// terrain, collision, weapon/ammo tables) reads the same layer.
#pragma once

#include "file_io.h"
#include "retail_mission_files.h"
#include "retail_paths.h"

#include <base/resource_index/resource_index.h>
#include <formats/mission/bms.h>
#include <runtime/mission/runtime_boot.h>

#include <cstdint>
#include <string>
#include <utility>
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
	if (!tree.empty() && test_io::read_file(join(tree, name), out)) {
		served_by = "OPENNOVA_JO_ASSETS";
		index.scan(install);
		return !out.empty();
	}
	return false;
}

// False (with `error`) when no layer carries `name` or it does not parse;
// `served_by` names the layer that did.
inline bool open_mission(opennova::testrig::RetailMissionRig &rig, const std::string &install,
                         const std::string &name, std::string &error, std::string &served_by) {
	std::vector<uint8_t> bytes;
	if (!read_mission(install, name, rig.index, bytes, served_by)) {
		error = name + " is on no mount under " + install +
		        " and not loose under OPENNOVA_JO_ASSETS";
		return false;
	}
	opennova::bms::File parsed;
	std::string parse_error;
	if (!opennova::bms::parse(bytes.data(), bytes.size(), parsed, parse_error)) {
		error = name + " did not parse: " + parse_error;
		return false;
	}
	opennova::mission::BootFileSource files;
	files.has_file = [&rig](const std::string &file) { return rig.index.has_file(file); };
	files.read_file = [&rig](const std::string &file, std::vector<uint8_t> &out) {
		return rig.index.read_file(file, out);
	};
	const size_t dot = name.rfind('.');
	rig.open_document(std::move(parsed), dot == std::string::npos ? name : name.substr(0, dot),
	                  std::move(files));
	rig.root_dir = install;
	return true;
}

} // namespace retail
