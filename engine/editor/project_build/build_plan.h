#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/graph/reference_kinds.h>
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
	std::string relative_path; // the project file's path, project-relative (what a finding about it names)
	uint64_t size_bytes = 0;
	// Where a loose file lands in the build directory, '/'-separated and relative: its name beside
	// the archives, or under `expansion/<b>/` in an expansion's build (ADR 0046 S16). Unused for an
	// archive's entry.
	std::string build_path;
	AssetKind kind = AssetKind::Unknown; // what the scan made of it
};

struct BuildArchive {
	ArchiveSlot slot = ArchiveSlot::Resource; // an expansion's <b>.pff takes Localres's and Resource's
	// Its path in the build directory, '/'-separated and relative: "language.pff", or an expansion's
	// "expansion/<b>/<b>L.pff".
	std::string file_name;
	std::vector<BuildEntry> entries; // sorted by normalized logical name
};

// What the build makes (ADR 0046 S16): the game standalone, its three boot-table archives at the
// build's root, which a launch on the build directory mounts in place of the install's; or the
// expansion `expansion` of the game install `install` (its target game's `game`), the folder
// `expansion/<expansion>/` with its two archives and its loose files, which a launch with
// `/exp <expansion>` mounts over the install's base game.
struct BuildTarget {
	std::string expansion; // "" for the standalone game
	std::string install;   // the game install the expansion plays over ("" when none is set)
	std::string game;      // the project's target game (gameprofile code): the base's SCR key
	bool is_expansion() const { return !expansion.empty(); }
};

struct BuildPlan {
	BuildTarget target;
	// Standalone: always the three boot-table archives, in slot order. An expansion: always its two,
	// <b>L.pff then <b>.pff, even empty (the game opens the pair by name).
	std::vector<BuildArchive> archives;
	std::vector<BuildEntry> loose;      // copied beside the archives (an expansion's into its folder)
	// An expansion's files the game reads from the install's folder alone (ExpansionPlace::RootOnly):
	// never built; the build compares each with the base's and says so where it differs
	// (build.expansion.root_only). `build_path` is its project path.
	std::vector<BuildEntry> root_only;
	std::vector<Diagnostic> diagnostics;
	// The diagnostics that refuse it, in their order (blocks_build over the plan's base: an expansion's
	// gate reads the base game's names, ADR 0046 S16); build_blockers answers them.
	std::vector<Diagnostic> blockers;
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
// again. `target` says what the build makes (the standalone game when it names no expansion); `base`,
// an expansion's base game's names (the session's base listing), without which an expansion does not
// build (build.expansion.base_missing), and over which its gate reads (blocks_build); `shipped`, the
// files packed as the game ships them, whose findings that they do not serialize gate nothing (null:
// every such finding gates).
// Whether the game's mission list lists a file of this name: a mission (.bms) or a map project (.npj,
// .npz) [orig: Mission_BuildMapListFromPFF @ 0x562910].
bool lists_as_mission(const std::string &name);

BuildPlan plan_build(const ProjectPaths &paths, const AssetScan &scan, const RequirementReport &requirements,
                     const std::vector<Diagnostic> &document_findings, const BuildTarget &target = BuildTarget(),
                     const BaseNames *base = nullptr, const ShippedFiles *shipped = nullptr);

// The plan's own findings over the scan alone, which plan_build adds to the gate it reads (each one the gate
// holds already it leaves): an archive the project holds (refused), a player's own file (left out), a
// NovaWorld screen of a name no archive holds (left out: the game never reads it), a name no archive can
// store (refused). The Problems rows hold them in the gate (project/project_findings.h), so a build
// is refused only for rows Problems shows and marks, before any build.
// `expansion` the project's own (its name, "" for a standalone project): where it builds a file, so which
// names an archive must store (ADR 0046 S16).
std::vector<Diagnostic> plan_scan_findings(const AssetScan &scan, const std::string &expansion = std::string());
// The same of one file (the rule plan_scan_findings runs over every file): true with the finding for a file
// the build refuses or leaves out with a word (Files' card says it as the build would), false for one it
// packs or copies, or leaves out without one.
bool plan_file_finding(const AssetEntry &asset, const std::string &expansion, Diagnostic &out);
// Where a build of the project (as the expansion `expansion`, "" for the standalone game) puts a file, in a
// sentence, from the decision the plan makes: its own finding on the file (plan_file_finding: an archive it
// refuses, a player's file it leaves out, a name no archive stores), else its placement (the archive it
// packs it into, the expansion's archive, the path it copies it to loose), else why it leaves it out. The
// one rule Files' card and a file's page say it by; `packed`, whether a build packs or copies it.
struct BuildPlaceWords {
	std::string words;
	bool packed = false;
};
BuildPlaceWords build_place_words(const AssetEntry &asset, const std::string &expansion = std::string());
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
