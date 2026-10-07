#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <editor/documents/model_item.h>
#include <editor/model/edit.h>

namespace opennova::editor {

class CanvasRequests;
struct AssetEntry;
struct SessionView;

// Placing a model no item of the project draws yet (ADR 0046 DI-12): a mission places items, and the game
// draws a model only as some item's graphic, so the editor makes that item first, in the catalog the
// project's items are in, then places it. Two gestures reach it: a model's file let go over a mission's
// picture (MissionViewport::drop), and a model's "Place in mission" (the model viewport's place_in_mission
// command), which arms the Place tool of the mission last active with the model's item.

// The item made for a model: the row the catalog gains, in one batch (one undo step of that file).
struct ModelItemPlan {
	std::string catalog; // the catalog's project path: where the project's items are (problem_fixes.h's defining_file)
	std::string model;   // the model's project path
	std::string graphic; // what the row's graphic names: the model's logical name less ".3di", as items.def writes it
	std::string name;    // the item's name: the graphic, and " 2", " 3" ... after it where an item has that name
	int64_t id = 0;      // the project's next free item id (free_project_item_id)
	ModelItemFacts facts; // its TYPE, from the model's parts (documents/model_item.h)
	// The catalog's batch: an Add of an item named `name`, then on what it made its `id`, its `type` and its
	// `graphic`; no class and no hp (model_item.h says why).
	std::vector<Edit> edits;
};

// The item a model no item draws takes, planned over the project as it stands (its open documents' edits
// included): false, with why, for a model whose item the editor cannot make from the model alone (a person, a
// vehicle, a mounted weapon: model_item.h), a model that does not read, a project with no item catalog, a
// graphic name longer than the game keeps.
bool plan_model_item(const SessionView &view, const AssetEntry &model, ModelItemPlan &out, std::string &error);

// What the plan makes, in words: "item oncrate2 (100020), a decoration (...), in items.def".
std::string model_item_plan_words(const ModelItemPlan &plan);

// The lowest item id the project leaves free, as a catalog's Add picks one (documents/def_table.h's
// free_item_id): from 100000 up, past every id the engine keeps (formats/def/reserved_items.h), every id an
// item of the project has (the graph's items, and the rows of `catalog` while it is open, its unsaved ones
// included) and every id a file of the project names (a mission's entity, a spawn list's: a new item of that
// id would quietly become what it names).
int64_t free_project_item_id(const SessionView &view, const std::string &catalog);

// The mission a model's Place in mission arms Place in: the active document where it is a mission, else the
// nearest mission open on Back's list (the one last active), else the one mission open; "" none.
std::string place_in_mission_target(const SessionView &view);

// The model viewport's place_in_mission command over the model at `model` (a project path): its item (made
// first where none draws it, as plan_model_item makes it; the one that draws it; several refused, naming
// each), the mission (place_in_mission_target) made active, and its Place tool armed with the item (a
// SetViewport of its options: tool place, item), said on the status line. False, with why: no mission open,
// several items, an item the editor cannot make.
bool plan_place_in_mission(const SessionView &view, const std::string &model, CanvasRequests &out, std::string &error);

} // namespace opennova::editor
