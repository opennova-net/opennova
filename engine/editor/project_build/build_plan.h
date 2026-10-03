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
	bool ok = false;                    // false when a diagnostic blocks the build (blocks_build)
	// The file the build keeps each file's content hash in, by the size and last write it was
	// hashed at (the project's `.opennova/build_cache.json`, machine-local: S13 A8), so a build
	// reads only the files that changed since; "" keeps none, every file then hashed.
	std::string hash_cache;
	// Every file read again, whatever the cache says, which it then keeps afresh (the build
	// request's `rehash`, the command line's --rehash).
	bool rehash = false;
};

// `document_findings` is the document validation over this scan
// (graph/project_validation.h validate_project) the caller already has: the session's
// last validation's gate, or the command line's one pass. The plan does not validate
// again. `target` says what the build makes (the standalone game when it names no expansion); `base`,
// an expansion's base game's names (the session's base listing), without which an expansion does not
// build (build.expansion.base_missing), and over which its gate reads (blocks_build).
BuildPlan plan_build(const ProjectPaths &paths, const AssetScan &scan, const RequirementReport &requirements,
                     const std::vector<Diagnostic> &document_findings, const BuildTarget &target = BuildTarget(),
                     const BaseNames *base = nullptr);

} // namespace opennova::editor
