#include <editor/project/expansion_name.h>

#include <base/gameprofile/required_resources.h>
#include <base/io/strutil.h>
#include <editor/assets/asset_registry.h>
#include <editor/project/expansion_files.h>
#include <runtime/mission/mission_sidecars.h>

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
	// An installed expansion is a folder the game already mounts by its name: the length binds it, and
	// that it is a folder's name of `expansion\` (the editor's view mounts it through the file system,
	// never a command line, and a player's `/exp` can quote a name with a space [orig:
	// Terrain_TokenizeConfigLine @ 0x53cc51]).
	if (use == ExpansionNameUse::BuildsOn) {
		for (const char c : name) {
			if (static_cast<unsigned char>(c) < 0x20)
				return quoted(name) + " has a control character, which no folder's name can hold.";
			if (std::string_view("\\/:*?\"<>|").find(c) != std::string_view::npos)
				return quoted(name) + " has '" + std::string(1, c) + "', which no folder's name can hold.";
		}
		if (name == "." || name == "..") return quoted(name) + " names no folder of expansion\\ of its own.";
		return std::string();
	}
	// The Mods list registers no folder whose name starts with a dot [orig: Expansion_ScanAndRegister
	// @ 0x4a444b..0x4a4450, `cmp cFileName[0], '.'`]: the expansion would mount with /exp alone.
	if (name.front() == '.')
		return quoted(name) + " starts with a dot: the game's Mods list never lists such a folder, so players could "
		                      "not choose it there.";
	for (const char c : name) {
		// One /exp token [orig: Terrain_TokenizeConfigLine @ 0x53cb60: ' ', '\t' and ',' split outside
		// quotes @ 0x53cc44, '"' toggles quoting and is never kept @ 0x53cc51, ';' ends the line
		// @ 0x53cc31]. Stricter than the game, by choice: a quoted `/exp "my mod"` mounts, but every
		// launch line, shortcut and server configuration that names the expansion unquoted would split
		// it, so the project's own name keeps to one bare token.
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
	if (!logical_name_fits_archive("M" + std::string(name) + ".bin") || !logical_name_fits_archive(std::string(name) + "L.lwf"))
		return quoted(name) + " is " + std::to_string(name.size()) +
		       " characters: the expansion's music script M" + std::string(name) + ".bin and its sound bank " +
		       std::string(name) + "L.lwf must fit the archives' 16-character names, so its name holds 11.";
	// The project's own files the name forms (expansion_files.h) must not be files the game reads by those
	// names for its own (the manifest's): the project holds one file of a name, and the game would read
	// it as both (a name "game" makes game.bin, which the game reads as its menu's table).
	for (const ExpansionFile &file : expansion_files(std::string(name))) {
		if (file.row->fixed) continue;
		const gameprofile::RequiredResource *own = gameprofile::gameprofile_required_resource_find(file.name.c_str());
		if (own && !(own->flags & gameprofile::RES_F_PATTERN))
			return quoted(name) + " would name " + file.row->what + " " + file.name + ", a file the game reads as " +
			       own->name + " for its own: give the expansion another name.";
	}
	return std::string();
}

std::string expansion_name_mission_problem(std::string_view name, const std::vector<std::string> &files) {
	if (name.empty()) return std::string();
	const std::vector<ExpansionFile> formed = expansion_files(std::string(name));
	for (const std::string &file : files) {
		// The mission list's files [orig: Mission_BuildMapListFromPFF @ 0x562910: .bms, .npj and .npz].
		if (!strutil::ends_with_icase(file, ".bms") && !strutil::ends_with_icase(file, ".npj") &&
		    !strutil::ends_with_icase(file, ".npz"))
			continue;
		for (const mission::Sidecar &sidecar : mission::sidecars()) {
			const std::string by_mission = mission::sidecar_name(file, sidecar);
			const std::string alternate = mission::sidecar_alternate_name(file, sidecar);
			for (const ExpansionFile &own : formed) {
				if (own.row->fixed) continue;
				if (!strutil::iequals(own.name, by_mission) && (alternate.empty() || !strutil::iequals(own.name, alternate)))
					continue;
				return quoted(name) + " would name " + own.row->what + " " + own.name + ", the file the game reads by the "
				       "mission " + file + "'s name [orig: Game_StartMission @ 0x524360]: give the expansion another name.";
			}
		}
	}
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
	if (expansion.name.empty() && expansion.builds_on.empty() && !expansion.on_base_project()) return true;
	if (!strutil::iequals(target_game, kDefaultTargetGame)) {
		error = make_finding(CoreFinding::ProjectExpansionUnsupported, DiagnosticSeverity::Error,
		                     "Expansions are witnessed for Joint Operations only: a project for \"" + target_game +
		                             "\" builds as the game itself.");
		return false;
	}
	if (expansion.name.empty() && expansion.on_base_project()) {
		error = make_finding(CoreFinding::ProjectFieldInvalid, DiagnosticSeverity::Error,
		                     "A project builds on the base game's project " + quoted(expansion.base_project) +
		                             " only as an expansion: a standalone project is a base game of its own. Give the "
		                             "project's expansion a name.");
		return false;
	}
	// An expansion of a project's base game builds on that game alone: the game mounts one expansion at a
	// time (`/exp` and `/mod` are one name, the last one wins [orig: Game_ParseCommandLineAndInit @
	// 0x4a76ac]), so it cannot stand over an installed expansion and a project's base too.
	if (expansion.on_base_project() && !expansion.builds_on.empty()) {
		error = make_finding(CoreFinding::ProjectFieldInvalid, DiagnosticSeverity::Error,
		                     "A project builds on the base game's project " + quoted(expansion.base_project) +
		                             " or on the installed expansion " + quoted(expansion.builds_on) +
		                             ", never both: the game mounts one expansion over one base game.");
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
