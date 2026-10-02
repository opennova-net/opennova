#pragma once

#include <string>

#include <base/io/strutil.h>
#include <editor/assets/asset_registry.h>
#include <editor/graph/reference_kinds.h>
#include <editor/model/value.h>
#include <formats/mns/mns.h>

// The spellings the asset graph compares names in: one home for the resolver, the
// extractors and the rename transaction.
namespace opennova::editor::graph_names {

// A file name as the scan keys it (normalized_logical_name).
inline std::string key(const std::string &value) { return normalized_logical_name(value); }
// A symbol as string ids and style variables compare it.
inline std::string upper(const std::string &value) { return strutil::to_upper(value); }
// A value that names a style variable (the whole value one "%NAME%", mns::is_variable_reference)
// rather than a file or a literal.
inline bool is_style_reference(const std::string &value) { return mns::is_variable_reference(value); }
// The variable a style reference names, as the graph keys it ("%def_text_fg%" -> DEF_TEXT_FG).
inline std::string style_variable(const std::string &value) { return upper(mns::variable_name(value)); }
// A symbol's name as the graph keys it, by its kind's name_case (reference_kinds): the whole
// name without case for the names the game's lookups compare that way (a string id, a style
// variable, a menu screen or window, a model's user point: stricmp over the whole string, no
// space trimmed [orig: CUIScene_SelectNodeByName @ 0x63b6b0; CWnd_FindChildByName @ 0x646850;
// ItemDef_GetBoneMaskByName @ 0x49ea40, the stricmp @ 0x49ea7b]), an item id as written, any
// other name as a file name. A name is keyed as given: a style variable's %NAME% spelling is
// read at the boundary that takes one (style_variable, find_definition).
inline std::string symbol_name(ReferenceKind kind, const std::string &name) {
	switch (reference_row(kind).name_case) {
	case NameCase::NoCase: return upper(name);
	case NameCase::Exact: return name;
	case NameCase::FileName: break;
	}
	return key(name);
}

} // namespace opennova::editor::graph_names
