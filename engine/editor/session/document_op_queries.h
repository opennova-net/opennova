#pragma once

// The queries that plan an op on an open document's content (S15, S17): a model's surfaces and a script's
// help, their params and choices, kept apart from the query table's other rows (session/editor_queries.cpp,
// which names these in its rows).

#include <cstddef>
#include <string>

#include <base/io/json.h>
#include <editor/session/editor_queries.h>
#include <editor/session/query_support.h>

namespace opennova::editor {

const char *surface_op_choice(size_t index);
const char *surface_flag_choice(size_t index);
const char *script_op_choice(size_t index);

inline constexpr QueryChoices kSurfaceChoices[] = {
	{ "op", surface_op_choice },
	{ "flag", surface_flag_choice },
};

inline constexpr QueryParam kModelSurfacesParams[] = {
	{ "path", QueryJson::String, false, nullptr, "An open model by its project-relative path or its logical name; left out, the "
			"active document." },
	{ "op", QueryJson::String, true, nullptr,
			"materials (the surfaces and flags a material takes by name, and a page of the model's materials, "
			"each its bullet faces' surface in words), set (the edits that give every face made from the material "
			"the surface), flag (the edits that set or clear a flag on every face made from it), faces (a page of "
			"its faces whose surface is not its common one)." },
	{ "id", QueryJson::Integer, false, nullptr, "The material by its identity (set, flag, faces)." },
	{ "surface", QueryJson::Integer, false, nullptr,
			"set's surface: a face byte, 0 to 23 by the game's names (materials lists them); any other byte is "
			"played as obj." },
	{ "flag", QueryJson::String, false, nullptr, "flag's flag: both_sides, bullets_pass or front_only." },
	{ "on", QueryJson::Boolean, false, "true", "flag's: set it (true) or clear it (false)." },
	{ "offset", QueryJson::Integer, false, "0", kQueryOffsetDoc },
	{ "limit", QueryJson::Integer, false, "100", kQueryLimitDoc },
};

inline constexpr QueryChoices kScriptChoices[] = { { "op", script_op_choice } };

inline constexpr QueryParam kScriptAssistParams[] = {
	{ "path", QueryJson::String, false, nullptr,
			"An open script (complete, hover, definition) or mission (mission_script) by its project-relative path "
			"or its logical name; left out, the active document." },
	{ "op", QueryJson::String, true, nullptr,
			"complete (what may complete the word at line, column), hover (what the word there is), definition "
			"(the request that goes where it is defined), mission_script (the script a mission's name finds)." },
	{ "line", QueryJson::Integer, false, "1", "The place's line, from 1." },
	{ "column", QueryJson::Integer, false, "1", "The place's column, from 1 (complete: the caret, the word ending there)." },
	{ "offset", QueryJson::Integer, false, "0", kQueryOffsetDoc },
	{ "limit", QueryJson::Integer, false, "100", kQueryLimitDoc },
};

io::JsonValue answer_model_surfaces(const QueryContext &context, const QueryArgs &args, std::string &error);
io::JsonValue answer_script_assist(const QueryContext &context, const QueryArgs &args, std::string &error);

} // namespace opennova::editor
