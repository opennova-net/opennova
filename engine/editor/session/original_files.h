#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <editor/assets/install_view.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_document.h>

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
// (`ready`), nor in a file of a name the install was not validated for (`covered`, where `scoped`: the
// check validates the install for the names the project's rows are about; unscoped, every name).
struct OriginalData {
	bool ready = false;
	bool scoped = false;
	std::set<std::string> covered; // the names it was validated for, normalized (where `scoped`)
	std::map<std::string, std::map<std::string, size_t>> findings;
	bool covers(const std::string &name) const { return !scoped || covered.count(name) != 0; }
	bool operator==(const OriginalData &o) const {
		return ready == o.ready && scoped == o.scoped && covered == o.covered && findings == o.findings;
	}
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
// The install is listed through the view the caller names (assets/install_view.h: its archives' files and
// the loose files the game reads beside them, with `/exp` for a project that builds on an installed
// expansion and under the project's names for one that builds as an expansion, ADR 0046 S16; for a stock
// expansion on its base game's project, the game install with that expansion mounted:
// SessionCore::original_install), each file by its logical name and read through the view (ProjectPaths::files), then validated as the session
// validates the project (ProjectValidation, then the document types' project checks over the install's
// files: a menu's screens compiled as the game draws them), every step within a budget of bytes, its own
// graph, cache and checks (nothing of the session's is copied or read). It validates only the files of
// the names the project's rows are about (want's `names`: a ValidationScope, with the missions whose
// terrain findings land on a terrain, an environment or overcast.def among them), which a validation of
// every file finds the same of: the asset graph still reads every file it reads, for the cross-file
// findings. The run is kept (its listing, graph, files' findings and checks; the mount let go between
// rounds, the install's archives free for a patch): a name a later row is about is validated then, in a round
// over the same graph and cache, and a finding of a name not yet covered is the modder's until it is
// (OriginalData::covered). It never depends on the project's files: an edit, a save, an import or a build
// validates nothing again but the name a row is about for the first time. A Refresh (a new scan) looks at the
// install's folders the view reads again (its own, the mounted expansion's), and an install that moved there (a
// patch, an expansion's archive among them) is validated again; another view (another install, game or expansion)
// forgets it.
class OriginalFiles {
public:
	OriginalFiles();
	~OriginalFiles();
	OriginalFiles(const OriginalFiles &) = delete;
	OriginalFiles &operator=(const OriginalFiles &) = delete;

	// The install to judge against, by the view of it (`install`, an empty root for none: SessionCore::
	// original_install), for the project (`document`, what the install is validated as); `scan` the session's
	// scan, a new one of which (a Refresh, a save) looks at the install's folder again; `names` the files the
	// project's rows are about (normalized logical names the view serves), each validated once (those already
	// wanted kept). Another view or folder state forgets what was found and starts over.
	void want(const InstallSpec &install, const std::shared_ptr<const ProjectDocument> &document, const void *scan,
	          const std::vector<std::string> &names);
	// Goes on within `bytes` (what it reads, each file at least kValidationFileCost: at least one
	// step); true when nothing is left to do.
	bool step(uint64_t bytes);
	// Nothing left to do: no install, or every name wanted covered (an install that does not mount covers
	// none, and never will).
	bool settled() const {
		return phase_ == Phase::Idle || (phase_ == Phase::Done && (!mounted_ || pending_.empty()));
	}
	// What it found: a new instance whenever it moves (then generation() moves too), never null.
	const std::shared_ptr<const OriginalData> &data() const { return data_; }
	uint64_t generation() const { return generation_; }
	// For the tests and the measure: how many times it validated the install (a round over the names wanted
	// then), of how many files whose own findings it made (the scope) among how many it listed, and the last
	// round's time in its steps (milliseconds of the steady clock; the first's with the listing).
	size_t validations() const { return validations_; }
	size_t files() const { return files_; }
	size_t listed() const { return listed_; }
	double last_ms() const { return last_ms_; }
	// The project closed: everything forgotten, the install let go.
	void clear();

private:
	enum class Phase : uint8_t { Idle, Mount, Scan, Validate, Checks, Rows, Done };
	struct Run;

	void start();
	void publish(OriginalData data);
	std::string folder_state() const;
	// A round over the names wanted: the scope of their files, validated over the run's graph and cache.
	void begin_round();
	// The scope's missions, once the graph holds the install: those whose terrain findings land on a file of the
	// scope (a terrain's, an environment's, every one's for overcast.def).
	void add_scope_missions();

	InstallSpec install_;
	std::shared_ptr<const ProjectDocument> document_;
	std::string folder_; // the state of the install's folders the view reads when it was validated (folder_state)
	const void *scan_ = nullptr;
	Phase phase_ = Phase::Idle;
	std::unique_ptr<Run> run_;
	bool mounted_ = false;           // the install mounted (else nothing is ever covered)
	std::set<std::string> wanted_;   // every name wanted since the run started, normalized
	std::set<std::string> covered_;  // those the last round published
	std::set<std::string> pending_;  // wanted, not covered
	std::shared_ptr<const OriginalData> data_;
	uint64_t generation_ = 0;
	size_t validations_ = 0;
	size_t files_ = 0;
	size_t listed_ = 0;
	double last_ms_ = 0.0;
};

} // namespace opennova::editor
