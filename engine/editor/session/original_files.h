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

namespace opennova {
class FileSource;
class Vfs;
}

namespace opennova::editor {

class AssetGraph;
class DocumentBase;
class ProjectChecks;
class ValidationCache;
struct ProjectDocument;
struct ProjectPaths;

// A finding as the game's own data's fold compares it: its code, the record it is on (by its path,
// the names from the row down: a record keeps it when its fields change or it moves among its
// siblings), the field and what it names; a text document's finding with no record by its message.
// The line is left out: an edit above a finding moves its line, not the finding.
std::string original_finding_key(const Diagnostic &d);

// What the check of the game's own data found (ADR 0046 S15, decided per finding since the UX round's
// problems lane): the files the findings are about that the project holds exactly as the game install
// serves them, every finding of which is the original's too; and the files the install serves that the
// project holds otherwise (an edit saved, or one unsaved), each with the findings its install copy
// makes standing in for it in the project, by key and how many of each. A finding of such a file is
// the game's own while its key is among them, as many times as they hold it: one edit no longer makes
// every finding the file had the modder's, only those the edit brought.
struct OriginalData {
	std::set<std::string> files;
	std::map<std::string, std::map<std::string, size_t>> findings;
	bool operator==(const OriginalData &o) const { return files == o.files && findings == o.findings; }
	bool operator!=(const OriginalData &o) const { return !(*this == o); }
};

// What a baseline reads of the project as the session's last validation left it: its paths, its graph,
// its files' own findings and its project checks (each copied or asked, never changed), and the files as
// the game looks them up (a project check's renders read them).
struct OriginalProject {
	const ProjectPaths &paths;
	const AssetGraph &graph;
	const ValidationCache &cache;
	const ProjectChecks &checks;
	const FileSource &files;
};

// The project's files that are the game's own data (ADR 0046 S15, Names): a file the project holds
// exactly as the game install serves it (asset_import's read_served over mount_retail, what an import of
// the install copies), so a Problems row about it is the original's too, not the modder's, and Problems
// shows it apart. Only the files a finding is about are checked, each once while its size and last
// write stand; the install is mounted while files wait to be checked and let go when none does (its
// archives closed: a patch may write them). A file the install does not serve, an install that cannot be
// mounted, and a file that cannot be read are not the original's.
//
// Per finding (the UX round's problems lane): a file the install serves that the project holds
// otherwise, or holds open with unsaved edits, is judged finding by finding against its install copy.
// The baseline is the project validated as it would be with the game's own copies of those files: the
// session's graph and its files' own findings copied once into a shadow of their own, the install copies
// standing in as open documents for the files that differ, each file's own findings and the use checks
// and the graph's findings made over it as the project's are, and the project checks asked of each
// install copy alone (ProjectChecks::findings_of: a menu's screens compiled as the game draws them). The
// shadow is kept while such files stand and brought to the project after each validation (an update
// over the same files reads only what changed); it goes when no file is held otherwise.
class OriginalFiles {
public:
	OriginalFiles();
	~OriginalFiles();
	OriginalFiles(const OriginalFiles &) = delete;
	OriginalFiles &operator=(const OriginalFiles &) = delete;

	// What to check: the install's folder (`install`, "" for none) and the project (`document` for its
	// game, `root` its folder), the scan (each file's logical name, size and last write), the findings
	// (the files they are about, their `asset`: project-relative paths) and the open documents (one with
	// unsaved edits is judged per finding). What it found of a file that still stands is kept; an
	// install or a project that moved forgets everything. The baseline is brought up again after the
	// files are checked.
	void want(const std::string &install, const std::shared_ptr<const ProjectDocument> &document,
	          const std::string &root, const std::shared_ptr<const AssetScan> &scan,
	          const std::vector<Diagnostic> &findings, const std::vector<std::shared_ptr<const DocumentBase>> &open);
	// Checks files within `bytes` (what it reads of the project and of the install; at least one file),
	// then brings the baseline up over `project` in a step of its own. True when nothing is left to do.
	bool step(uint64_t bytes, const OriginalProject &project);
	bool settled() const { return queue_.empty() && !baseline_due_; }
	// What it found: a new instance whenever it moves (then generation() moves too), never null.
	const std::shared_ptr<const OriginalData> &data() const { return data_; }
	uint64_t generation() const { return generation_; }
	// How many files it has read to check (for the tests: a file that stands is checked once), and how
	// many times it brought a baseline up over a shadow.
	size_t checked() const { return checked_; }
	size_t baselines() const { return baselines_; }
	// Whether it holds a shadow of the project (for the tests: none while every file is as the install's).
	bool shadowed() const { return shadow_graph_ != nullptr; }
	// The project closed: everything forgotten, the install let go.
	void clear();

private:
	// A file to check, or one checked: as the scan listed it, and what the check found.
	struct File {
		std::string path; // project-relative
		std::string name; // its logical name, what the install serves it by
		AssetKind kind = AssetKind::Unknown;
		uint64_t size = 0;
		int64_t modified = 0;
		bool served = false;   // the install serves a file of its name
		bool original = false; // byte for byte the install's
	};
	// An install copy as a document, kept while the project's file stands as it was checked, and what the
	// project checks made of it while the scan it was made over stands.
	struct Copy {
		bool from_install = false; // read from the install (else from the project's file, the same bytes)
		uint64_t size = 0;
		int64_t modified = 0;
		std::shared_ptr<DocumentBase> document; // null: it did not load
		const AssetScan *checked_over = nullptr;
		std::vector<Diagnostic> checked;
	};
	bool mount();
	void compare(uint64_t bytes);
	void baseline(const OriginalProject &project);
	std::shared_ptr<DocumentBase> copy_of(const File &file);
	void publish(OriginalData data);

	std::string install_;
	std::shared_ptr<const ProjectDocument> document_;
	std::string root_;
	std::shared_ptr<const AssetScan> scan_;
	std::vector<std::shared_ptr<const DocumentBase>> open_;
	std::set<std::string> asked_; // the files the findings are about
	std::unique_ptr<Vfs> game_;
	bool mount_tried_ = false;
	std::map<std::string, File> known_;
	std::vector<File> queue_;
	bool baseline_due_ = false;
	std::map<std::string, Copy> copies_;
	std::unique_ptr<AssetGraph> shadow_graph_;
	std::unique_ptr<ValidationCache> shadow_cache_;
	std::shared_ptr<const OriginalData> data_;
	uint64_t generation_ = 0;
	size_t checked_ = 0;
	size_t baselines_ = 0;
};

} // namespace opennova::editor
