#include "blank_makers.h"

#include <editor/model/core_finding.h>
#include <formats/mns/mns.h>

namespace opennova::editor {

namespace {

struct StyleDefine {
	const char *name;
	const char *value;
};

// The variables the blank screens' %VAR% references resolve through: the game pastes a
// variable's value wherever a menu names it [orig: NapiXML_ExpandVariablesInText @
// 0x63a000] and reads no variable by name. The three font names are what the blank
// menus' FONT NAMEs use (each loads as a .fnt when a window parses [orig:
// CFontCache_LoadOrGetFont @ 0x652f70]); the rest are the text, trim and selection
// colours.
const StyleDefine k_defines[] = {
	{ "DEF_FONTNAME", "Arial16n.fnt" },
	{ "DEF_FONTNAME_LG", "Arial16b.fnt" },
	{ "IMPACT_FONTNAME", "Impac38b.fnt" },
	{ "DEF_TEXT_FG", "FFFFFFFF" },
	{ "DEF_TEXT_MOUSEOVER_FG", "FFFFC040" },
	{ "DEF_TEXT_SELECTED_FG", "FFFF8000" },
	{ "DEF_TEXT_DISABLED_FG", "FF545252" },
	{ "TRIM_COLOR", "FF808080" },
	{ "ITEM_SELECTED_BG", "80FF8000" },
	{ "COLOR_BLACK", "FF000000" },
	{ "SEMIOPAQUE_BLACK", "80000000" },
};

} // namespace

bool make_blank_menu_style(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &error) {
	// The list the game reads (mns::KeyValueList), one variable a line, CR LF (the only line end the game reads
	// without stopping): the stylesheet's writer, from the list alone.
	mns::KeyValueList list;
	for (const StyleDefine &define : k_defines) list.nodes.push_back({define.name, define.value});
	std::string text, write_error;
	if (!mns::write(list, text, write_error)) {
		error = make_finding(CoreFinding::BlankStyle, DiagnosticSeverity::Error, write_error, request.logical_name);
		return false;
	}
	out.assign(text.begin(), text.end());
	return true;
}

// brand.mns: read after menu_style.mns into the same variables, so a name it defines replaces menu_style.mns's value
// [orig: Menu_InitShellResources @ 0x552616]. Blank: no variable, an empty file.
bool make_blank_brand_style(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &) {
	out.clear();
	return true;
}

} // namespace opennova::editor
