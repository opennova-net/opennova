#include "blank_makers.h"

#include <editor/session/finding_codes.h>
#include <formats/mns/mns_document.h>

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

std::string style_header(const BlankRequest &request, const char *what) {
	return "// " + std::string(what) + " of " +
	       (request.project_title.empty() ? std::string("the project") : request.project_title) + ".\r\n";
}

} // namespace

bool make_blank_menu_style(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &error) {
	// Through the lossless document model: a comment header, then one define per
	// variable, CRLF (the only line end the game reads without stopping).
	mns::Document doc = mns::Document::parse(
	        style_header(request, "Menu style") + "// The fonts and colors the screens reference as %NAME%.\r\n");
	for (const StyleDefine &define : k_defines) {
		std::string add_error;
		if (!doc.add_define(define.name, define.value, -1, std::string(), &add_error)) {
			error = make_finding(CoreFinding::BlankStyle, DiagnosticSeverity::Error, add_error, request.logical_name);
			return false;
		}
	}
	out = doc.serialize();
	return true;
}

// brand.mns: read after menu_style.mns into the same variables, so a name it defines
// replaces menu_style.mns's value [orig: Menu_InitShellResources @ 0x552616]. Blank: a
// comment header and nothing else.
bool make_blank_brand_style(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &) {
	const std::string text = style_header(request, "Brand style") +
	                         "// A variable defined here replaces menu_style.mns's value of the same name.\r\n";
	out.assign(text.begin(), text.end());
	return true;
}

} // namespace opennova::editor
