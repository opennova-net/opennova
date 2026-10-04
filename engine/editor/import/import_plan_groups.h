#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <editor/assets/asset_kind.h>
#include <editor/import/import_plan.h>

namespace opennova::editor {

// The import plan's rows by what they come for (the UX round's project lane): the import dialog's tree
// and the wire's `groups`. Each chosen file is a group of its own (its row the group's one row), and a
// file the walk found goes under the group of the file that wanted it (ImportPlanRow::needed_by), in a
// group of its kind there: main.mnu > Sound bank > Wave, items.def > Model > Texture, so the textures
// a thousand models bring are one line to read and one check to leave out. A plan with no file found
// and more than kKindGroupsPast chosen (every file of the game install) goes by kind alone, the
// largest kind first. The rows not found are in no group (the dialog lists them apart).
struct ImportPlanGroup {
	static constexpr size_t kNone = SIZE_MAX;
	size_t parent = kNone; // the group it sits in; kNone for a top group
	size_t depth = 0;
	// The chosen file's row the group comes for (a chosen file's own group: that row); kNone for a
	// kind's group of a plan by kind alone, and for the files whose wanting file the plan lacks.
	size_t root = kNone;
	AssetKind kind = AssetKind::Unknown; // its rows' kind
	std::vector<size_t> rows;            // its own rows, in the plan's order
	std::vector<size_t> children;        // the groups in it, in the order their first rows were planned
	size_t files = 0;                    // its rows and those of every group in it
	uint64_t bytes = 0;                  // their sizes as stored
	// Whether it is a chosen file's own group (its one row that file), else a kind's.
	bool chosen() const { return root != kNone && depth == 0; }
};

// Past this many files chosen with none found, the plan goes by kind.
inline constexpr size_t kKindGroupsPast = 20;

// The plan's groups, a parent before its children (every group's index above its parent's).
std::vector<ImportPlanGroup> import_plan_groups(const ImportPlan &plan);

} // namespace opennova::editor
