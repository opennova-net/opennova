// Open a retail mission into the shared rig the way the OPENNOVA_JO_DIR tests
// read missions (retail_mission.h): the base mount first, then each expansion
// the install carries, then the loose OPENNOVA_MISSION_CORPUS copy. The rig's
// own index stays scanned on the layer that served the mission and becomes
// the kernel's asset source, so the kernel boot (seat specs, .adm clips,
// terrain, collision, weapon/ammo tables) reads the same layer.
#ifndef OPENNOVA_TEST_RETAIL_MISSION_OPEN_H
#define OPENNOVA_TEST_RETAIL_MISSION_OPEN_H

#include "retail_mission.h"
#include "retail_mission_files.h"

#include <formats/mission/bms.h>
#include <runtime/mission/runtime_boot.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace retail {

// False (with `error`) when no layer carries `name` or it does not parse;
// `served_by` names the layer that did.
inline bool open_mission(opennova::testrig::RetailMissionRig &rig, const std::string &install,
                         const std::string &name, std::string &error, std::string &served_by) {
	std::vector<uint8_t> bytes;
	if (!read_mission(install, name, rig.index, bytes, served_by)) {
		error = name + " is on no mount under " + install +
		        " and not under OPENNOVA_MISSION_CORPUS";
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
	rig.set_asset_index(&rig.index);
	rig.root_dir = install;
	return true;
}

} // namespace retail

#endif // OPENNOVA_TEST_RETAIL_MISSION_OPEN_H
