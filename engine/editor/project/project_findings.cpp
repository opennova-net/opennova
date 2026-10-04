#include <editor/project/project_findings.h>

#include <iterator>
#include <set>

#include <base/gameprofile/required_resources.h>
#include <editor/assets/asset_kind.h>
#include <editor/documents/project_checks.h>
#include <editor/graph/project_validation.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_files.h>
#include <editor/project_build/build_plan.h>
#include <editor/requirements/requirement_words.h>
#include <runtime/mission/mission_sidecars.h>

namespace opennova::editor {

namespace {

// The Problems row of a file the game reported missing when it booted: the project's (no
// file of it is at fault), naming the manifest row's role and required name when the name
// is one, and what the game does without it.
Diagnostic boot_finding(const std::string &name) {
	const gameprofile::RequiredResource *row = gameprofile::gameprofile_required_resource_find(name.c_str());
	std::string message = "The game could not find " + name + " when it started.";
	// What the game does without it, in plain words (requirements/requirement_words.h).
	const std::string without = row != nullptr ? requirement_without(row->role) : std::string();
	if (!without.empty()) message += " " + without;
	Diagnostic d = make_finding(CoreFinding::PlayBootMissing, DiagnosticSeverity::Error, message);
	d.subject = RequirementSubject{ row != nullptr ? row->role : "", row != nullptr ? row->name : name };
	return d;
}

// The files the game finds by a mission's name alone (ADR 0046 S14, mission::sidecars) that nothing
// reads: a script, a tile placement or a dialog bank whose mission the project does not hold, which
// no file names (a script another runs, a tile file a terrain names) and the game opens by no
// fixed name (game.wac, server.wac). What a mission's rename leaves behind, or an import of the
// file without its mission: a note on the file, never a gate.
void sidecar_notes(const AssetScan &scan, const AssetGraph &graph, std::vector<Diagnostic> &rows) {
	// The base names of the project's missions, as every reader takes them (to the first dot: a
	// mission "op.v2.bms" opens "op.wac").
	std::set<std::string> missions;
	for (const AssetEntry &entry : scan.entries)
		if (entry.kind == AssetKind::Mission) missions.insert(normalized_logical_name(mission::mission_base_name(entry.logical_name)));
	for (const AssetEntry &entry : scan.entries) {
		if (entry.kind != AssetKind::Script && entry.kind != AssetKind::TileInfo && entry.kind != AssetKind::DialogBank)
			continue;
		if (gameprofile::gameprofile_required_resource_find(entry.logical_name.c_str()) != nullptr) continue;
		const std::string base = mission::mission_base_name(entry.logical_name);
		const std::string mission = base + ".bms";
		const std::string wanted = normalized_logical_name(entry.logical_name);
		bool by_name = false;
		for (const mission::Sidecar &sidecar : mission::sidecars())
			by_name = by_name || normalized_logical_name(mission::sidecar_name(mission, sidecar)) == wanted;
		if (!by_name || missions.count(normalized_logical_name(base)) || !graph.referrers_of_file(entry.relative_path).empty())
			continue;
		rows.push_back(make_finding(CoreFinding::MissionSidecarUnused, DiagnosticSeverity::Info,
		                            "The game opens " + entry.logical_name + " with the mission " + mission +
		                                    ", which the project does not hold, and no file names it: nothing reads it.",
		                            entry.relative_path));
	}
}

} // namespace

ProjectFindings compose_project_findings(const ProjectFindingsInput &input, AssetGraph &graph, ValidationCache &cache,
                                         ProjectChecks &checks, const FileSource &files) {
	refresh_project_findings(input, graph, cache, checks, files);
	return collect_project_findings(input, graph, cache, checks);
}

bool refresh_project_findings(const ProjectFindingsInput &input, AssetGraph &graph, ValidationCache &cache,
                              ProjectChecks &checks, const FileSource &files) {
	const ValidationInput validation{ input.paths, input.project, input.scan, input.open };
	const bool files_moved = refresh_project(validation, graph, cache);
	// After the files' own findings: a check reads which files' records their own checks read.
	const bool checks_moved = checks.update({ validation, cache, files });
	return files_moved || checks_moved;
}

ProjectFindings collect_project_findings(const ProjectFindingsInput &input, const AssetGraph &graph,
                                         const ValidationCache &cache, const ProjectChecks &checks) {
	ProjectFindings out;
	std::vector<Diagnostic> &rows = out.rows;
	const ValidationInput validation{ input.paths, input.project, input.scan, input.open };
	std::vector<Diagnostic> documents = project_rows(validation, graph, cache);
	rows.reserve(input.scan.diagnostics.size() + input.requirements.diagnostics.size() +
			input.boot_missing.size() + input.play.size() + documents.size() +
			input.open_findings.size() + checks.findings_size() + input.build.size());
	rows.insert(rows.end(), input.scan.diagnostics.begin(), input.scan.diagnostics.end());
	rows.insert(rows.end(), input.requirements.diagnostics.begin(),
			input.requirements.diagnostics.end());
	sidecar_notes(input.scan, graph, rows);
	for (const std::string &name : input.boot_missing)
		rows.push_back(boot_finding(name));
	rows.insert(rows.end(), input.play.begin(), input.play.end());
	out.gate_begin = rows.size();
	rows.insert(rows.end(), std::make_move_iterator(documents.begin()),
			std::make_move_iterator(documents.end()));
	rows.insert(rows.end(), input.open_findings.begin(), input.open_findings.end());
	// The build's own word on the files (an archive in the project, a name no archive stores, a player's
	// file left out): rows of the gate, so a build is refused only for what Problems shows and marks.
	const std::vector<Diagnostic> plan = plan_scan_findings(input.scan, input.project.expansion.name);
	rows.insert(rows.end(), plan.begin(), plan.end());
	out.gate_end = rows.size();
	checks.append_findings(rows);
	rows.insert(rows.end(), input.build.begin(), input.build.end());
	return out;
}

} // namespace opennova::editor
