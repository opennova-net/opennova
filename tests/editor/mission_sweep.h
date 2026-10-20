#pragma once
// The missions the editor's mission tests sweep, in place of every mission the game install ships (its
// 115, the base game's and each expansion's): the minted fixtures/editor/missions
// (tests/fixtures/editor_missions_gen.cpp: every record kind and field form those 115 hold, and every
// table at the most records one of them holds in it), the format's own mission fixtures (fixtures/bms:
// the synthetic missions, and the loadout chunks as five shipped missions damage them, D-EVT-7, which
// the format's corpus row mints) and the base game's own missions (assets/, expansions/). Every file is
// required: one the tree lacks is a failure, so the sweep never shrinks to what happens to be there.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "common/file_io.h"
#include "common/test_paths.h"

namespace mission_sweep {

struct Mission {
	std::string path; // repo-relative
	std::string name; // its file name
	std::vector<uint8_t> bytes;
	// Whether the file holds a loadout chunk the writer writes otherwise (mission.rewrite_differs, D-EVT-7):
	// an unedited save writes the format's canonical bytes of it, not the file's.
	bool rewrites = false;
};

struct Entry {
	const char *path;
	bool rewrites;
};

inline const std::vector<Entry> &entries() {
	static const std::vector<Entry> list = {
	        {"fixtures/editor/missions/logic.bms", false},
	        {"fixtures/editor/missions/records.bms", false},
	        {"fixtures/editor/missions/scale.bms", false},
	        {"fixtures/bms/synth_logic.bms", false},
	        {"fixtures/bms/synth_dense.bms", false},
	        // A mission whose area triggers take a shipped training mission's shape (mission_set_gen.cpp's, which
	        // mission_mis_idempotency reads).
	        {"fixtures/bms/synth_areas.bms", false},
	        {"assets/onjo_m1.bms", false},
	        {"expansions/onjo1/missions/onjo1_m1.bms", false},
	        // The loadout chunks five shipped missions carry damaged (D-EVT-7, bms-event-runtime-re.md 6.3a), the
	        // format's corpus row's (mission_corpus): a record cut short before the terminator (ASP_G8a.bms,
	        // ASR_C2A.bms), strings past the terminator (TDH_G1A.bms), no terminator (TKR_G3A.bms, TKX_G5B.bms).
	        {"fixtures/bms/loadout_cut_record.bms", true},
	        {"fixtures/bms/loadout_past_terminator.bms", true},
	        {"fixtures/bms/loadout_unterminated.bms", true},
	};
	return list;
}

// Every mission of the sweep the tree holds, read; each one it lacks (or holds as an unpulled LFS pointer)
// named in `missing`.
inline std::vector<Mission> load(std::vector<std::string> &missing) {
	std::vector<Mission> out;
	const std::string root = std::string(test_paths_repo_root(__FILE__)) + "/";
	for (const Entry &entry : entries()) {
		Mission mission;
		mission.path = entry.path;
		mission.name = mission.path.substr(mission.path.find_last_of('/') + 1);
		mission.rewrites = entry.rewrites;
		if (!test_io::read_file(root + entry.path, mission.bytes) || mission.bytes.empty() ||
		    test_io::is_lfs_pointer(mission.bytes)) {
			missing.push_back(entry.path);
			continue;
		}
		out.push_back(std::move(mission));
	}
	return out;
}

// The sweep's missing files said, and whether there were none.
inline bool none_missing(const std::vector<std::string> &missing) {
	for (const std::string &path : missing) std::fprintf(stderr, "the sweep's mission %s is not in the tree\n", path.c_str());
	return missing.empty();
}

// The minted missions' names (fixtures/editor/missions), whose forms the tests hold to account by name.
inline bool minted(const Mission &mission, const char *name) {
	return mission.path == std::string("fixtures/editor/missions/") + name;
}

} // namespace mission_sweep
