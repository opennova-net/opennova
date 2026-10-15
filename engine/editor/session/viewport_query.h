#pragma once

// The viewport query (S13 V7): a document's viewport read by op (state, items, hit, box, notes, render,
// palette), its params and choices, kept apart from the query table's other rows (session/editor_queries.cpp,
// which names these in its row).

#include <cstddef>
#include <string>

#include <base/io/json.h>
#include <editor/session/editor_queries.h>
#include <editor/session/query_support.h>

namespace opennova::editor {

const char *viewport_op_choice(size_t index);
const char *viewport_kind_choice(size_t index);

inline constexpr QueryParam kViewportParams[] = {
	{ "path", QueryJson::String, false, nullptr,
			"An open document by its project-relative path or its logical name, or the texture Files "
			"selects (S18); left out, the active one." },
	{ "kind", QueryJson::String, false, nullptr,
			"The viewport's kind (menu, model, script, mission, texture); left out, the one the document shows "
			"in: the Preview's kind that shows it (a menu's; a model's, over a model, a clip or an "
			"animation table), else its Main view." },
	{ "op", QueryJson::String, true, nullptr,
			"What is read: state (the envelope, a page of its items and by the same page its "
			"notes), items or notes (a page of one), hit (what lies under x, y), box (what the box from "
			"x, y to x2, y2 takes, as a marquee over it: a menu's windows it touches), render (one row "
			"of the document as the kind renders it apart: a menu's screen, as the render check "
			"compiled it), palette (what a mission's Place tool places: the items by name in their "
			"groups, the recently placed first, those whose name, id or model holds text)." },
	{ "x", QueryJson::Number, false, nullptr,
			"hit's point across (box's first corner), in the viewport's units (a menu's 800x600 "
			"design units, a model's picture pixels): a number a float holds." },
	{ "y", QueryJson::Number, false, nullptr, "hit's point down, as x." },
	{ "x2", QueryJson::Number, false, nullptr, "box's other corner across, as x (x and y its first)." },
	{ "y2", QueryJson::Number, false, nullptr, "box's other corner down, as x." },
	{ "row", QueryJson::Integer, false, nullptr, "render's row, by its identity (a menu's screen)." },
	{ "text", QueryJson::String, false, nullptr,
			"palette's search: the items whose name, id or model's file holds it, case aside (left "
			"out, every item)." },
	{ "offset", QueryJson::Integer, false, "0", kQueryOffsetDoc },
	{ "limit", QueryJson::Integer, false, "100", kQueryLimitDoc },
};

inline constexpr QueryChoices kViewportChoices[] = {
	{ "op", viewport_op_choice },
	{ "kind", viewport_kind_choice },
};

io::JsonValue answer_viewport(const QueryContext &context, const QueryArgs &args, std::string &error);

} // namespace opennova::editor
