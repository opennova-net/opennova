// The synthetic JO person rig staged where a ResourceIndex finds it: the
// minted person model (fixtures/threedi/synth/person.3di) beside its clip set
// (fixtures/anim/rig19.adm and every clip it names, minted by
// tests/fixtures/minimal_bad_gen.cpp on the same rig, tests/fixtures/person_rig.h),
// copied flat into one directory as a game folder holds its loose files.
// Infrastructure only.
#pragma once

#include <string>
#include <vector>

#include "common/file_io.h"
#include "common/test_paths.h"

namespace rig19 {

// The clip set's files under fixtures/anim.
inline const std::vector<std::string> &clip_files() {
	static const std::vector<std::string> kFiles = {"rig19.adm", "rig19_rst.bad", "rig19_idle.bad",
		"rig19_crch.bad", "rig19_walk.bad", "rig19_rld1.bad", "rig19_rld2.bad", "rig19_dthf.bad",
		"rig19_dthr.bad", "rig19_dthb.bad", "rig19_dthl.bad"};
	return kFiles;
}

// Copy person.3di and the clip set into `dir` (an existing directory). False
// when a fixture does not read or a copy does not write.
inline bool stage(const std::string &dir) {
	std::vector<uint8_t> bytes;
	if (!test_io::read_file(test_paths_fixture("threedi/synth/person.3di"), bytes) ||
			!test_io::write_file(dir + "/person.3di", bytes))
		return false;
	for (const std::string &name : clip_files())
		if (!test_io::read_file(test_paths_fixture("anim/" + name), bytes) || !test_io::write_file(dir + "/" + name, bytes))
			return false;
	return true;
}

} // namespace rig19
