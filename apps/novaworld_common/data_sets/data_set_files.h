#pragma once

// The files a data set's tree holds, by kind (ADR 0064): what opennova-serve reads from an
// install and nothing else (ADR 0053 d3's list, split per product). A base game's tree is the
// retail boot table, resource.pff, localres.pff and language.pff (required: Vfs::mount_game
// mounts no install without them), with version.txt (the product's version, the updater's
// precheck file), score.ini and _NSTMOUT.TXT (the two loose root files the server reads) and
// gt.ssc (the gate tag override, ADR 0055 d3) when the install has them. An expansion's tree
// is its pair, <code>.pff and <code>L.pff, and its version.txt (the checksum joiners compare,
// vfs_expansion_version_checksum), required, with gt.ssc when present. The admin page keeps
// only these out of a picked folder, the upload route refuses any other name, and the
// worker's finalize checks the required ones are there. Service policy, not a port.

#include "data_set_repository.h"

#include <string>
#include <string_view>
#include <vector>

namespace opennova::novaworld_server::hosted {

struct TreeFileRule {
	std::string name; // the canonical spelling, as the tree stores it
	bool required = false;
};

// The kind's files, required ones first. `expansion_code` names an expansion's pair.
std::vector<TreeFileRule> tree_file_rules(DataSetKind kind, std::string_view expansion_code);

// The canonical spelling of `name` among the kind's files (compared without case: a retail
// install spells _NSTMOUT.TXT and jox01L.pff in its own case), or "" when the kind takes no
// file of that name.
std::string canonical_tree_file(DataSetKind kind, std::string_view expansion_code, std::string_view name);

// The required names `names` (canonical spellings) lacks.
std::vector<std::string> missing_tree_files(DataSetKind kind, std::string_view expansion_code,
		const std::vector<std::string> &names);

// The two archive names a tree's mission list is read from (mission_catalog's pair walk): a
// base's localres.pff with language.pff, an expansion's <code>.pff with <code>L.pff.
struct MissionPair {
	std::string missions;
	std::string texts;
};
MissionPair mission_pair_of(DataSetKind kind, std::string_view expansion_code);

} // namespace opennova::novaworld_server::hosted
