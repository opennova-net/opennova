#include "blank_makers.h"

#include <sstream>

#include <editor/project/project_files.h>
#include <formats/env/env.h>

namespace opennova::editor {

// A new environment (ADR 0046 S20, so a mission can be made on a terrain made from images): the
// environment writer's authoring template (env::make_default_config: noon by default, three keyframes
// at 00:00, 12:00 and 23:59 with their sky, fog, sun and ground colours, fog to 1000 units, the
// stock cloud maps and celestial models by name), named after the file. A missing or empty
// environment is no failure to the game, which runs on its defaults then [orig:
// Environment_LoadTimeOfDayConfig @ 0x57DCA3]; this one gives a mission a sky and light to start
// from. The cloud maps and models it names are the stock game's: a project without them imports
// them, or names its own.
bool make_blank_environment(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &error) {
	env::Config config = env::make_default_config();
	config.name = utf8_of(path_of(request.logical_name).stem());
	std::ostringstream text;
	std::string why;
	if (!env::save_env(text, config, why)) {
		out.clear();
		error = make_finding(CoreFinding::BlankEnvironment, DiagnosticSeverity::Error, why, request.logical_name);
		return false;
	}
	const std::string bytes = text.str();
	out.assign(bytes.begin(), bytes.end());
	return true;
}

} // namespace opennova::editor
