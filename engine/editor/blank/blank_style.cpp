#include "blank_makers.h"

#include <formats/mns/mns_document.h>

namespace opennova::editor {

namespace {

struct StyleDefine {
	const char *name;
	const char *value;
};

// The variables the menu shell and the shipped screens read by name: the three font
// slots [orig: the DEF_FONTNAME family menu_style.mns names, HUD_InitAllFonts
// @ 0x51ee20 loads the files] and the text/trim/selection colors the widgets'
// %VAR% references resolve through [orig: NapiXML_ExpandVariablesInText @ 0x63a000].
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
	{ "DEF_IMAGE_DEFAULT_BG", "FF000000" },
};

} // namespace

bool make_blank_menu_style(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &error) {
	// Through the lossless document model: a comment header, then one define per
	// variable, CRLF (the document's default line ending for authored lines).
	mns::Document doc = mns::Document::parse(
	        "// Menu style of " + (request.project_title.empty() ? std::string("the project") : request.project_title) +
	        ": the fonts and colors the screens reference as %NAME%.\r\n");
	for (const StyleDefine &define : k_defines) {
		std::string add_error;
		if (!doc.add_define(define.name, define.value, -1, std::string(), &add_error)) {
			error = make_diagnostic(DiagnosticSeverity::Error, "blank.style", add_error, request.logical_name);
			return false;
		}
	}
	out = doc.serialize();
	return true;
}

} // namespace opennova::editor
