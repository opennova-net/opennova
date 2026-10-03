#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/model/diagnostic.h>

namespace opennova::editor {

class InstallView;
struct ProjectDocument;

// The project's files that are the game's own data, byte for byte (ADR 0046 S15, Names): a file the
// project holds exactly as the game install serves it (the install as the project imports it,
// assets/install_view.h: what an import of the install copies), so a Problems row about it is the original's too, not the
// modder's, and Problems shows it apart. Only the files a finding is about are checked, each once while
// its size and last write stand; the install is mounted while files wait to be checked and let go when
// none does (its archives closed: a patch may write them). A file the install does not serve, an
// install that cannot be mounted, and a file that cannot be read are not the original's.
class OriginalFiles {
public:
	OriginalFiles();
	~OriginalFiles();
	OriginalFiles(const OriginalFiles &) = delete;
	OriginalFiles &operator=(const OriginalFiles &) = delete;

	// What to check: the install's folder (`install`, "" for none) and the project (`document` for its
	// game, `root` its folder), the scan (each file's logical name, size and last write) and the
	// findings (the files they are about, their `asset`: project-relative paths). What it found of a
	// file that still stands is kept; an install or a project that moved forgets everything, as does a
	// change of the project's expansion or of the one it builds on (the install's view moved: ADR 0046
	// S16, install_spec).
	void want(const std::string &install, const std::shared_ptr<const ProjectDocument> &document,
	          const std::string &root, const AssetScan &scan, const std::vector<Diagnostic> &findings);
	// Checks files within `bytes` (what it reads of the project and of the install; at least one file).
	// True when none is left to check.
	bool step(uint64_t bytes);
	bool settled() const { return queue_.empty(); }
	// The files found the original's (project-relative paths): a new set whenever it moves (then
	// generation() moves too), never null.
	const std::shared_ptr<const std::set<std::string>> &files() const { return files_; }
	uint64_t generation() const { return generation_; }
	// How many files it has read to check (for the tests: a file that stands is checked once).
	size_t checked() const { return checked_; }
	// The project closed: everything forgotten, the install let go.
	void clear();

private:
	// A file to check, or one checked: as the scan listed it, and what the check found.
	struct File {
		std::string path; // project-relative
		std::string name; // its logical name, what the install serves it by
		uint64_t size = 0;
		int64_t modified = 0;
		bool original = false;
	};
	bool mount();
	void publish();

	std::string install_;
	std::shared_ptr<const ProjectDocument> document_;
	std::string root_;
	std::unique_ptr<InstallView> game_;
	bool mount_tried_ = false;
	std::map<std::string, File> known_;
	std::vector<File> queue_;
	std::shared_ptr<const std::set<std::string>> files_;
	uint64_t generation_ = 0;
	size_t checked_ = 0;
};

} // namespace opennova::editor
