#include "blank_makers.h"

#include <formats/mnu/mnu.h>

namespace opennova::editor {

namespace {

std::string xml_escape(const std::string &text) {
	std::string out;
	for (const char c : text) {
		switch (c) {
		case '&': out += "&amp;"; break;
		case '<': out += "&lt;"; break;
		case '>': out += "&gt;"; break;
		case '"': out += "&quot;"; break;
		default: out.push_back(c);
		}
	}
	return out;
}

// The startup screen, authored from scratch. The engine loads main.mnu and selects the
// "STARTUP" node [orig: Menu_InitShellResources @ 0x552651 -> CUIScene_SelectNodeByName
// @ 0x63b6b0]; the MAIN window carries the 800x600 design frame under a 75px header
// band the way the shipped screens do; labels take their font and colors from the
// stylesheet variables so the blank menu_style.mns styles it. The Exit button carries
// no action: exit buttons are shell Commands bound by Window name
// (docs/mnu/menu-re.md, ADR 0001; the shell's exit_control_names).
//
// The screen depends only on files "Create all missing" makes. Its labels are literal
// text, not string ids: menutxt.bin is an OPTIONAL row of the manifest (retail falls back
// to literals without it), so it is never created for a new project, and a label looked
// up in a table that is not there draws its raw key.
std::string startup_screen_xml(const std::string &title) {
	return "<SCREEN>\n"
	       "\t<NAME>STARTUP</NAME>\n"
	       "\t<MUSICVAR>1</MUSICVAR>\n"
	       "\t<WINDOW type=\"window\" name=\"MAIN\">\n"
	       "\t\t<APPEARANCE type=\"custom\" state=\"default\"></APPEARANCE>\n"
	       "\t\t<POSITION>\n"
	       "\t\t\t<LEFT>0</LEFT>\n"
	       "\t\t\t<TOP>75</TOP>\n"
	       "\t\t\t<RIGHT>800</RIGHT>\n"
	       "\t\t\t<BOTTOM>525</BOTTOM>\n"
	       "\t\t</POSITION>\n"
	       "\t\t<FONT>\n"
	       "\t\t\t<NAME>%DEF_FONTNAME_LG%</NAME>\n"
	       "\t\t\t<DEFAULT_FG>%DEF_TEXT_FG%</DEFAULT_FG>\n"
	       "\t\t\t<MOUSEOVER_FG>%DEF_TEXT_MOUSEOVER_FG%</MOUSEOVER_FG>\n"
	       "\t\t\t<SELECTED_FG>%DEF_TEXT_SELECTED_FG%</SELECTED_FG>\n"
	       "\t\t\t<DISABLED_FG>%DEF_TEXT_DISABLED_FG%</DISABLED_FG>\n"
	       "\t\t</FONT>\n"
	       "\t\t<WINDOW type=\"static\" name=\"TITLE\">\n"
	       "\t\t\t<APPEARANCE state=\"default\"></APPEARANCE>\n"
	       "\t\t\t<POSITION>\n"
	       "\t\t\t\t<LEFT>0</LEFT>\n"
	       "\t\t\t\t<TOP>120</TOP>\n"
	       "\t\t\t\t<RIGHT>800</RIGHT>\n"
	       "\t\t\t</POSITION>\n"
	       "\t\t\t<STRING justify=\"CENTER\">" + xml_escape(title) + "</STRING>\n"
	       "\t\t</WINDOW>\n"
	       "\t\t<WINDOW type=\"button\" name=\"EXIT\">\n"
	       "\t\t\t<APPEARANCE state=\"default\"></APPEARANCE>\n"
	       "\t\t\t<APPEARANCE state=\"mouseover\"></APPEARANCE>\n"
	       "\t\t\t<APPEARANCE state=\"selected\"></APPEARANCE>\n"
	       "\t\t\t<APPEARANCE state=\"disabled\"></APPEARANCE>\n"
	       "\t\t\t<POSITION>\n"
	       "\t\t\t\t<LEFT>340</LEFT>\n"
	       "\t\t\t\t<TOP>378</TOP>\n"
	       "\t\t\t\t<RIGHT>460</RIGHT>\n"
	       "\t\t\t</POSITION>\n"
	       "\t\t\t<STRING justify=\"CENTER\">Exit</STRING>\n"
	       "\t\t</WINDOW>\n"
	       "\t</WINDOW>\n"
	       "</SCREEN>\n";
}

} // namespace

bool make_blank_main_menu(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &error) {
	const std::string title = request.project_title.empty() ? std::string("NEW GAME") : request.project_title;
	// Through the document model: the authored screen parses, and the serializer's
	// canonical form (CRLF on disk) is what lands in the project.
	mnu::Document doc;
	std::string parse_error;
	if (!mnu::parse(startup_screen_xml(title), doc, parse_error)) {
		error = make_diagnostic(DiagnosticSeverity::Error, "blank.menu",
		                        "The startup screen did not parse: " + parse_error, request.logical_name);
		return false;
	}
	blank_text_to_bytes(mnu::serialize(doc, true, 2), out);
	return true;
}

} // namespace opennova::editor
