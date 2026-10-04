#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_document.h>
#include <editor/project_build/archive_routing.h>
#include <editor/requirements/requirements.h>

namespace opennova::editor {

// What one build will write (ADR 0046 d8): every packable asset routed into its
// archive, the loose files beside them, and the validation gate. A plan with blocking
// diagnostics (an error among the document findings, the scan's, an unmet Required row, a
// file the engine cannot store; blocks_build: an error whose code's row gates) is not run:
// Play and Export both refuse rather than pack a game that cannot boot. A missing reference
// is among the findings and blocks nothing (ADR 0046 S14: the shipped game's own files carry
// them and it runs). An import source and a file of no kind the game knows are left out
// (S13 A8): the first's outputs pack, the second the game never asks for.
struct BuildEntry {
	std::string logical_name;  // the engine-facing name (the archive entry name)
	std::string source_path;   // absolute path of the project file
	uint64_t size_bytes = 0;
};

struct BuildArchive {
	ArchiveSlot slot = ArchiveSlot::Resource;
	std::string file_name;           // "language.pff", ...
	std::vector<BuildEntry> entries; // sorted by normalized logical name
};

struct BuildPlan {
	std::vector<BuildArchive> archives; // always the three boot-table archives, in slot order
	std::vector<BuildEntry> loose;      // copied beside the archives
	std::vector<Diagnostic> diagnostics;
	bool ok = false;                    // false when a diagnostic blocks the build (blocks_build)
	// The file the build keeps each file's content hash in, by the size and last write it was
	// hashed at (the project's `.opennova/build_cache.json`, machine-local: S13 A8), so a build
	// reads only the files that changed since; "" keeps none, every file then hashed.
	std::string hash_cache;
	// Every file read again, whatever the cache says, which it then keeps afresh (the build
	// request's `rehash`, the command line's --rehash).
	bool rehash = false;
	// The project's id (ProjectDocument::project_id), which its build record names: a build reuses, prunes
	// and replaces only its own project's builds where several share a folder (Build to folder).
	std::string project;
	// The folder is the project's own (its default build folder, under its cache): a build recorded before
	// records named their project is taken for its own there, and for another project's anywhere else.
	bool own_folder = false;
};

// `document_findings` is the document validation over this scan
// (graph/project_validation.h validate_project) the caller already has: the session's
// last validation's gate, or the command line's one pass. The plan does not validate
// again.
BuildPlan plan_build(const ProjectPaths &paths, const AssetScan &scan, const RequirementReport &requirements,
                     const std::vector<Diagnostic> &document_findings);

// The plan's own findings over the scan alone, which plan_build adds to the gate it reads (each one the gate
// holds already it leaves): an archive the project holds (refused), a player's own file (left out), a name no
// archive can store (refused). The Problems rows hold them in the gate (project/project_findings.h), so a build
// is refused only for rows Problems shows and marks, before any build.
std::vector<Diagnostic> plan_scan_findings(const AssetScan &scan);
// The findings a plan is refused for (its diagnostics that block_build): what Problems marks "Blocks the
// build", what the build_gate query lists and what a refused build names, in the plan's order.
std::vector<Diagnostic> build_blockers(const BuildPlan &plan);

// A refusal in a few words, for a refused build's line and the status line: a required file by its name
// and what the game does without it ("keyhelp.bin is missing: the game shows "Unable to load keyboard map
// strings" and exits"), anything else by its message.
std::string blocker_words(const Diagnostic &d);
// Why a build is refused for it, citing the refusal it follows (ADR 0046 S14, the gate follows retail): a
// required file the boot exits or dead-ends without, as its manifest row witnessed it; a reference whose
// loader the game refuses to start without, as its kind's row cites it; else the editor's own integrity
// (a file it cannot read, write or store as it is: it does not pack what it cannot vouch for).
std::string blocker_reason(const Diagnostic &d);
// Whether a refusal follows the game's own (a required file the boot cannot go on without, a reference whose
// loader the game refuses to start without), as against the editor's integrity (a file it cannot read, write
// or store as it is).
bool blocker_is_the_games(const Diagnostic &d);
// A refused build's line: how many problems refuse it and the first few in words.
std::string refusal_words(const std::vector<Diagnostic> &blockers);

} // namespace opennova::editor
