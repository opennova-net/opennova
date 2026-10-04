#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <editor/model/diagnostic.h>

namespace opennova::editor {

struct ProjectDocument;

// A finding as the game's own data's fold compares it (the UX round's problems lane): its code, the
// record it is on as itself (Diagnostic::record_key, Document::record_identity: its kind and own name,
// else a digest of what it holds, so a record inserted, removed, moved or renamed beside it, or an owner
// renamed, leaves it as it was), the field and what it names. A finding that names a record but no key
// by its record's path, its numbers left out; a text document's finding of no record by its words, its
// numbers left out too (a count or a place in a message moves with an edit elsewhere). Never its line.
std::string original_finding_key(const Diagnostic &d);

// What the game install makes of its own files (ADR 0046 S15, decided per finding against the install
// as a whole since the UX round's problems lane): each file the install serves, by its logical name
// (normalized_logical_name), with the findings a validation of the whole install makes about it, by key
// and how many of each. A finding of the project is the game's own while its file's logical name holds
// its key, as many times as the install's holds it; nothing is before the install was validated
// (`ready`).
struct OriginalData {
	bool ready = false;
	std::map<std::string, std::map<std::string, size_t>> findings;
	bool operator==(const OriginalData &o) const { return ready == o.ready && findings == o.findings; }
	bool operator!=(const OriginalData &o) const { return !(*this == o); }
};

// The game's own data's baseline (ADR 0046 S15; the UX round's problems lane): the game install
// validated as a project of its own, once, for every finding of the project to be judged against what
// the install as a whole makes: a finding is the game's own only when the install, as it ships, makes
// it too, in the file of the same name. A file's own findings the install's copy of it makes; a
// reference the install's files do not resolve either (a missing texture the install lacks too) but
// not one whose target the install has (the modder's project lacks it: the modder's); what other files
// make of a name the install's files make too. So an edit never makes the original's findings of a
// file the modder's, and the modder's own effects on files they never touched are theirs.
//
// The install is listed as the game serves it (mount_retail: its archives' files, the loose files the
// game ships beside them), each file by its logical name and read through the mount (ProjectPaths::
// files), then validated as the session validates the project (ProjectValidation, then the document
// types' project checks over the install's files: a menu's screens compiled as the game draws them),
// every step within a budget of bytes, its own graph, cache and checks let go once the findings are
// keyed (nothing of the session's is copied or read). It never depends on the project: an edit, a
// save, an import or a build validates nothing again. A Refresh (a new scan) looks at the install's
// folder again, and an install that moved there (a patch) is validated again; another install or game
// forgets it.
class OriginalFiles {
public:
	OriginalFiles();
	~OriginalFiles();
	OriginalFiles(const OriginalFiles &) = delete;
	OriginalFiles &operator=(const OriginalFiles &) = delete;

	// The install to judge against (`install`, "" for none) for the project's game (`document`);
	// `scan` the session's scan, a new one of which (a Refresh, a save) looks at the install's folder
	// again. Another install, game or folder state forgets what was found and starts over.
	void want(const std::string &install, const std::shared_ptr<const ProjectDocument> &document, const void *scan);
	// Goes on within `bytes` (what it reads, each file at least kValidationFileCost: at least one
	// step); true when nothing is left to do.
	bool step(uint64_t bytes);
	bool settled() const { return phase_ == Phase::Idle || phase_ == Phase::Done; }
	// What it found: a new instance whenever it moves (then generation() moves too), never null.
	const std::shared_ptr<const OriginalData> &data() const { return data_; }
	uint64_t generation() const { return generation_; }
	// For the tests and the measure: how many times it validated an install, of how many files, the last
	// one's time in its steps and the longest step it took (milliseconds of the steady clock).
	size_t validations() const { return validations_; }
	size_t files() const { return files_; }
	double last_ms() const { return last_ms_; }
	double longest_step_ms() const { return longest_ms_; }
	// The project closed: everything forgotten, the install let go.
	void clear();

private:
	enum class Phase : uint8_t { Idle, Mount, Scan, Validate, Checks, Rows, Done };
	struct Run;

	void start();
	void publish(OriginalData data);
	std::string folder_state() const;

	std::string install_;
	std::shared_ptr<const ProjectDocument> document_;
	std::string game_;
	std::string folder_; // the install folder's state when it was validated (folder_state)
	const void *scan_ = nullptr;
	Phase phase_ = Phase::Idle;
	std::unique_ptr<Run> run_;
	std::shared_ptr<const OriginalData> data_;
	uint64_t generation_ = 0;
	size_t validations_ = 0;
	size_t files_ = 0;
	double last_ms_ = 0.0;
	double longest_ms_ = 0.0;
};

} // namespace opennova::editor
