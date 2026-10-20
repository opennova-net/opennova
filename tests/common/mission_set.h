// The minted mission set (fixtures/mission_set, tests/fixtures/mission_set_gen.cpp) staged as one
// loose game root, the root the world and mission tests that once booted shipped training missions
// open their missions from: the synthetic terrain (fixtures/terrain/tmap, but its items.def), the
// synthetic model set (fixtures/threedi/synth), the authored clips (fixtures/anim) and the mission
// set itself, copied flat into a scratch directory in that order (a later file of a name replaces
// an earlier one), which the kernel mounts as the game mounts loose files (MissionKernel::open's
// loose-tree leg). Infrastructure only.
#pragma once

#include "common/temp_dir.h"
#include "common/test_paths.h"

#include <filesystem>
#include <string>
#include <system_error>

namespace test_mission_set {

class Root {
public:
	explicit Root(const char *name) : dir_(name) {
		const std::string repo = test_paths_repo_root(__FILE__);
		ok_ = stage(repo + "/fixtures/terrain/tmap", "items.def") && stage(repo + "/fixtures/threedi/synth") &&
		      stage(repo + "/fixtures/anim") && stage(repo + "/fixtures/mission_set");
	}
	bool ok() const { return ok_; }
	std::string path() const { return dir_.root(); }

private:
	// Every regular file of `from` into the root, but `skip`.
	bool stage(const std::string &from, const char *skip = nullptr) {
		std::error_code ec;
		bool any = false;
		for (const auto &entry : std::filesystem::directory_iterator(from, ec)) {
			if (!entry.is_regular_file(ec)) continue;
			const std::filesystem::path name = entry.path().filename();
			if (skip != nullptr && name == skip) continue;
			std::filesystem::copy_file(entry.path(), dir_.path / name,
					std::filesystem::copy_options::overwrite_existing, ec);
			if (ec) return false;
			any = true;
		}
		return any && !ec;
	}

	test_temp::TempDir dir_;
	bool ok_ = false;
};

} // namespace test_mission_set
