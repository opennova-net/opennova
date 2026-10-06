#include "blank_makers.h"


#include <base/io/strutil.h>
#include <editor/project/project_files.h>
#include <formats/mnu/mnu.h>

namespace opennova::editor {

namespace {

// The MAIN window's font, which every window under it inherits: the stylesheet's
// variables, so the blank menu_style.mns styles it.
const char *const k_main_font =
        "\t\t<FONT>\n"
        "\t\t\t<NAME>%DEF_FONTNAME_LG%</NAME>\n"
        "\t\t\t<DEFAULT_FG>%DEF_TEXT_FG%</DEFAULT_FG>\n"
        "\t\t\t<MOUSEOVER_FG>%DEF_TEXT_MOUSEOVER_FG%</MOUSEOVER_FG>\n"
        "\t\t\t<SELECTED_FG>%DEF_TEXT_SELECTED_FG%</SELECTED_FG>\n"
        "\t\t\t<DISABLED_FG>%DEF_TEXT_DISABLED_FG%</DISABLED_FG>\n"
        "\t\t</FONT>\n";

// The MAIN window's pointer: the game's own pointer file (blank_pointer_name, made with the menu
// where the project has none: blank_companion), alpha-blended as every shipped screen's CURSOR
// asks. The original game hides the system pointer as it starts, so a screen whose windows name
// no CURSOR has no pointer at all [orig: Game_InitSubsystems @ 0x4a725a -> Game_HideCursorLoop
// @ 0x7612e0; CUIScene_DrawScreensAndCursor @ 0x63bf60]; a root window's CURSOR is the one every
// window under it shows [orig: CWnd_GetInheritedCursorTexture @ 0x646AD0] (docs/mnu/menu-re.md).
std::string main_cursor() {
	return std::string("\t\t<CURSOR>\n"
	                   "\t\t\t<FILE>") +
	       mnu::escape_text(blank_pointer_name()) +
	       "</FILE>\n"
	       "\t\t\t<FLAGS>STANDARD_TRANSPARENT</FLAGS>\n"
	       "\t\t</CURSOR>\n";
}

// The startup screen, authored from scratch. The engine loads main.mnu and selects the
// "STARTUP" node [orig: Menu_InitShellResources @ 0x552651 -> CUIScene_SelectNodeByName
// @ 0x63b6b0]; the MAIN window carries the 800x600 design frame under a 75px header
// band the way the shipped screens do; labels take their font and colors from the
// stylesheet variables so the blank menu_style.mns styles it. The Exit button carries
// no action: exit buttons are shell Commands bound by Window name
// (docs/mnu/menu-re.md, ADR 0001; the shell's exit_control_names).
//
// The screen depends only on files the required files' Create fixes make (the pointer made with
// it). Its labels are literal
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
	       "\t\t</POSITION>\n" +
	       std::string(k_main_font) + blank_menu_cursor() +
	       "\t\t<WINDOW type=\"static\" name=\"TITLE\">\n"
	       "\t\t\t<APPEARANCE state=\"default\"></APPEARANCE>\n"
	       "\t\t\t<POSITION>\n"
	       "\t\t\t\t<LEFT>0</LEFT>\n"
	       "\t\t\t\t<TOP>120</TOP>\n"
	       "\t\t\t\t<RIGHT>800</RIGHT>\n"
	       "\t\t\t</POSITION>\n"
	       "\t\t\t<STRING justify=\"CENTER\">" + mnu::escape_text(title) + "</STRING>\n"
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

// A menu file of its own (Create menu): one screen named after the file (extra.mnu
// makes EXTRA) whose MAIN window spans the whole 800x600 design frame, empty but for
// the pointer. An editor authoring default, not a retail layout; the title and the Exit
// button belong to the STARTUP screen of main.mnu alone.
std::string free_form_screen_xml(const std::string &screen) {
	return "<SCREEN>\n"
	       "\t<NAME>" + mnu::escape_text(screen) + "</NAME>\n"
	       "\t<WINDOW type=\"window\" name=\"MAIN\">\n"
	       "\t\t<APPEARANCE type=\"custom\" state=\"default\"></APPEARANCE>\n"
	       "\t\t<POSITION>\n"
	       "\t\t\t<LEFT>0</LEFT>\n"
	       "\t\t\t<TOP>0</TOP>\n"
	       "\t\t\t<RIGHT>800</RIGHT>\n"
	       "\t\t\t<BOTTOM>600</BOTTOM>\n"
	       "\t\t</POSITION>\n" +
	       std::string(k_main_font) + blank_menu_cursor() +
	       "\t</WINDOW>\n"
	       "</SCREEN>\n";
}

} // namespace

const char *blank_menu_font() {
	return k_main_font;
}

std::string blank_menu_cursor() {
	return main_cursor();
}

// Through the document model: the authored screen parses, and the serializer's
// canonical form (CRLF on disk) is what lands in the project.
bool blank_menu_bytes(const std::string &xml, const BlankRequest &request, std::vector<uint8_t> &out,
                      Diagnostic &error) {
	mnu::Document doc;
	std::string parse_error;
	if (!mnu::parse(xml, doc, parse_error)) {
		error = make_finding(CoreFinding::BlankMenu, DiagnosticSeverity::Error, "The new screen did not parse: " + parse_error,
		                     request.logical_name);
		return false;
	}
	blank_text_to_bytes(mnu::serialize(doc, true, 2), out);
	return true;
}

bool make_blank_main_menu(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &error) {
	const std::string title = request.project_title.empty() ? std::string("NEW GAME") : request.project_title;
	return blank_menu_bytes(startup_screen_xml(title), request, out, error);
}

bool make_blank_menu(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &error) {
	std::string screen = strutil::to_upper(utf8_of(path_of(request.logical_name).stem()));
	if (screen.empty()) screen = "MAIN";
	return blank_menu_bytes(free_form_screen_xml(screen), request, out, error);
}

} // namespace opennova::editor
