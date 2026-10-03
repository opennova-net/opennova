#include <editor/project/expansion_name.h>

#include <base/io/strutil.h>
#include <editor/assets/asset_registry.h>

namespace opennova::editor {

namespace {

// Windows' device names, which a folder of the name (or of the name before a dot) opens instead.
bool device_name(std::string_view name) {
	const std::string stem = strutil::to_upper(name.substr(0, name.find('.')));
	static constexpr const char *kDevices[] = { "CON", "PRN", "AUX", "NUL" };
	for (const char *device : kDevices)
		if (stem == device) return true;
	return stem.size() == 4 && (stem.compare(0, 3, "COM") == 0 || stem.compare(0, 3, "LPT") == 0) &&
	       stem[3] >= '1' && stem[3] <= '9';
}

std::string quoted(std::string_view name) { return "'" + std::string(name) + "'"; }

} // namespace

std::string expansion_name_problem(std::string_view name, ExpansionNameUse use) {
	if (name.empty()) return "An expansion needs a name.";
	if (name.size() > kExpansionNameMax)
		return quoted(name) + " is " + std::to_string(name.size()) +
		       " characters: the game holds an expansion's name in 31, and a longer one does not mount.";
	for (const char c : name) {
		// One /exp token [orig: Terrain_TokenizeConfigLine @ 0x53cb60: ' ', '\t' and ',' split outside
		// quotes @ 0x53cc44, '"' toggles quoting and is never kept @ 0x53cc51, ';' ends the line
		// @ 0x53cc31].
		if (c == ' ' || c == '\t' || c == ',' || c == '"' || c == ';')
			return quoted(name) +
			       " has a space, a tab, a comma, a quote or a semicolon: the game takes an expansion's name from its "
			       "command line as one word, which those end.";
		const unsigned char byte = static_cast<unsigned char>(c);
		if (byte < 0x21 || byte > 0x7e)
			return quoted(name) + " has a character outside printable ASCII: give the expansion a name of letters, "
			                      "digits and plain punctuation.";
		if (std::string_view("\\/:*?<>|").find(c) != std::string_view::npos)
			return quoted(name) + " has '" + std::string(1, c) + "', which no folder's name can hold.";
	}
	if (name.back() == '.') return quoted(name) + " ends with a dot, which Windows drops from a folder's name.";
	if (device_name(name)) return quoted(name) + " is a name Windows keeps for a device: no folder can have it.";
	// The project's own expansion's files the archives hold by its name [orig: Expansion_LoadAssets:
	// "M%s.bin" @ 0x4a491d (and "G%s.bin" @ 0x4a494a), "%sL.lwf" @ 0x4a4989].
	if (use == ExpansionNameUse::Own &&
	    (!logical_name_fits_archive("M" + std::string(name) + ".bin") ||
	     !logical_name_fits_archive(std::string(name) + "L.lwf")))
		return quoted(name) + " is " + std::to_string(name.size()) +
		       " characters: the expansion's music script M" + std::string(name) + ".bin and its sound bank " +
		       std::string(name) + "L.lwf must fit the archives' 16-character names, so its name holds 11.";
	return std::string();
}

bool check_expansion_name(const std::string &name, ExpansionNameUse use, Diagnostic &error) {
	const std::string problem = expansion_name_problem(name, use);
	if (problem.empty()) return true;
	error = make_finding(CoreFinding::ProjectFieldInvalid, DiagnosticSeverity::Error, problem);
	return false;
}

bool check_project_expansion(const std::string &target_game, const ProjectExpansion &expansion,
                             Diagnostic &error) {
	if (expansion.name.empty() && expansion.builds_on.empty()) return true;
	if (!strutil::iequals(target_game, kDefaultTargetGame)) {
		error = make_finding(CoreFinding::ProjectExpansionUnsupported, DiagnosticSeverity::Error,
		                     "Expansions are witnessed for Joint Operations only: a project for \"" + target_game +
		                             "\" builds as the game itself.");
		return false;
	}
	if (expansion.name.empty()) {
		error = make_finding(CoreFinding::ProjectFieldInvalid, DiagnosticSeverity::Error,
		                     "A project builds on the expansion " + quoted(expansion.builds_on) +
		                             " only as an expansion of its own: the game reads an expansion's files only "
		                             "under /exp. Give the project's expansion a name.");
		return false;
	}
	if (!check_expansion_name(expansion.name, ExpansionNameUse::Own, error)) return false;
	return expansion.builds_on.empty() || check_expansion_name(expansion.builds_on, ExpansionNameUse::BuildsOn, error);
}

void expansion_install_findings(const ProjectExpansion &expansion, const std::vector<std::string> &installed,
                                DiagnosticSeverity severity, std::vector<Diagnostic> &out) {
	if (expansion.standalone()) return;
	const auto has = [&installed](const std::string &name) {
		for (const std::string &each : installed)
			if (strutil::iequals(each, name)) return true;
		return false;
	};
	if (has(expansion.name))
		out.push_back(make_finding(CoreFinding::ProjectExpansionNameTaken, severity,
		                           "The game install has an expansion named " + quoted(expansion.name) +
		                                   " already: the project's would stand in for it. Give the project's "
		                                   "expansion another name."));
	if (!expansion.builds_on.empty() && !has(expansion.builds_on))
		out.push_back(make_finding(CoreFinding::ProjectExpansionNotInstalled, severity,
		                           "The project builds on the expansion " + quoted(expansion.builds_on) +
		                                   ", which the game install does not have."));
}

} // namespace opennova::editor
