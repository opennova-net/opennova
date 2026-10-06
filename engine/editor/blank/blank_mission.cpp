#include "blank_makers.h"


#include <base/io/strutil.h>
#include <editor/project/project_files.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/til/til_io.h>

namespace opennova::editor {

namespace {

// A file's name as a mission's header names it: without its extension (the game appends the
// terrain's .trn and the environment's .env itself), whichever way the picker gave it.
std::string base_name(const std::string &name, const char *extension) {
	return strutil::ends_with_icase(name, extension) ? name.substr(0, name.size() - std::char_traits<char>::length(extension))
	                                                 : name;
}

} // namespace

std::string blank_mission_title(const BlankRequest &request) {
	const std::string &title = request.value("title");
	return title.empty() ? utf8_of(path_of(request.logical_name).stem()) : title;
}

// A new mission (ADR 0046 S14): the header a mission the game lists and loads holds
// (mission::make_blank: the shipped missions' common values, cited there), named, on the terrain
// and under the environment asked for; no entity, area or event. Written through the mission
// writer: no retail byte.
bool make_blank_mission(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &error) {
	mission::BlankMission blank;
	blank.name = blank_mission_title(request);
	blank.terrain = base_name(request.value("terrain"), ".trn");
	blank.environment = base_name(request.value("environment"), ".env");
	bms::File file;
	std::string why;
	if (!mission::make_blank(file, blank, why) || !bms::write(file, out, why)) {
		out.clear();
		error = make_finding(CoreFinding::BlankMission, DiagnosticSeverity::Error, why, request.logical_name);
		return false;
	}
	return true;
}

// A mission's tile placement with no tile placed (an empty TilFile: the header and a count of none),
// written through the .til writer. The game reads <mission>.til as the mission's terrain loads, the
// terrain's own placement only where the mission has none [orig: Terrain_Init @ 0x60fbe0, the name @
// 0x60fd0c; Terrain_LoadTileInfoFile @ 0x60a740], and every shipped mission carries its own beside it.
bool make_blank_tile_info(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &error) {
	std::string why;
	if (save_til(TilFile(), out, why)) return true;
	out.clear();
	error = make_finding(CoreFinding::BlankMission, DiagnosticSeverity::Error, why, request.logical_name);
	return false;
}

// A script that does nothing yet: a comment naming it (the compiler reads `//` to the line's end).
bool make_blank_script(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &) {
	blank_text_to_bytes("// " + request.logical_name + "\n", out);
	return true;
}

} // namespace opennova::editor
