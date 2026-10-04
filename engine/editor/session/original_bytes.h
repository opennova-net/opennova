#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <editor/project/project_document.h>

namespace opennova::editor {

// The project's files that are the game's own bytes (ADR 0046 S16, the gate over the game's own bytes):
// each byte for byte what the game install serves under its name, as the project imports it (the install's
// view, assets/install_view.h: with `/exp` for a project that builds on an installed expansion, under the
// project's names for one that builds as an expansion), the decoded form an import copies. The build packs
// such a file as stored, so a finding that the editor's writer cannot write it back refuses nothing
// (graph/reference_kinds.h, ShippedFiles).
//
// Asked of the few files a build would refuse for not serializing, at the build and the build gate: what it
// read is kept by each file's size and last write, once that last write has settled (git's racy rule,
// io::file_stamp_settled), until the project, its install, its game or its expansion moves; the install's
// view is opened only when a file is not known, and let go when the call returns (a patch may write its
// archives). A file the install does not serve, an install that does not open and a file that cannot be
// read are not the game's own bytes.
class OriginalBytes {
public:
	// Which of `files` (project-relative paths under `root`) are the install's bytes.
	std::set<std::string> identical(const std::string &install, const ProjectDocument &document,
	                                const std::string &root, const std::vector<std::string> &files);
	// For the tests: how many files it has read since it was made.
	size_t reads() const { return reads_; }
	// The project closed: everything forgotten.
	void clear();

private:
	struct Known {
		uint64_t size = 0;
		int64_t written = 0;
		bool same = false;
	};
	std::string key_; // the project's root, the install, the game, the expansion
	std::map<std::string, Known> known_;
	size_t reads_ = 0;
};

} // namespace opennova::editor
