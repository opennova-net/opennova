// The mission type's findings (mission_validation.h): its table, and validate_file over a mission
// document: the source findings its parse made (a rewrite that differs, the events' runs and their
// order), then what the game makes of the records (ADR 0046 S14; the record checks land with the
// mission's use checks).
#include "mission_validation.h"

#include <iterator>
#include <string>

#include <editor/documents/mission_document.h>

namespace opennova::editor {

namespace {

constexpr const char *kRewriteSections = "with its sections as the game reads them";
constexpr const char *kRewriteRuns = "with each event's triggers and actions where the event stands";

constexpr FindingCodeEntry<MissionFinding> kFindingEntries[] = {
	{ MissionFinding::RewriteDiffers, { "mission.rewrite_differs", FindingFix::Rewrite, kRewriteSections } },
	{ MissionFinding::InvalidInput, { "mission.invalid_input", FindingFix::None, nullptr, true } },
	{ MissionFinding::EventOrder, { "mission.event_order", FindingFix::Rewrite, kRewriteRuns } },
	{ MissionFinding::SsnDuplicate, { "mission.ssn_duplicate" } },
	{ MissionFinding::ZoneDuplicate, { "mission.zone_duplicate" } },
	{ MissionFinding::ZoneDegenerate, { "mission.zone_degenerate" } },
	{ MissionFinding::ZoneId, { "mission.zone_id" } },
	{ MissionFinding::EventMissing, { "mission.event_missing" } },
	{ MissionFinding::MarkerMissing, { "mission.marker_missing" } },
	{ MissionFinding::PathCount, { "mission.path_count" } },
	{ MissionFinding::PathOneShot, { "mission.path_one_shot" } },
	{ MissionFinding::PathEmpty, { "mission.path_empty" } },
	{ MissionFinding::PathStart, { "mission.path_start" } },
	{ MissionFinding::GroupRange, { "mission.group_range" } },
	{ MissionFinding::PoolLimit, { "mission.pool_limit" } },
	{ MissionFinding::GameMode, { "mission.game_mode" } },
	{ MissionFinding::TriggerType, { "mission.trigger_type" } },
	{ MissionFinding::BoundingBox, { "mission.bounding_box" } },
};
static_assert(std::size(kFindingEntries) == static_cast<size_t>(MissionFinding::kCount),
              "every MissionFinding has exactly one row");
static_assert(finding_entries_well_formed(kFindingEntries),
              "the mission's rows follow MissionFinding's order, each token its own");
constexpr auto kFindingRows = finding_rows(kFindingEntries, FindingGroup::Missions);
static_assert(finding_rows_well_formed(kFindingRows), "every row of the table takes its group");

// The severity of a source finding: the events' runs block (an error); a rewrite that differs and
// a layout the writer lays out again are what Save does (an info each).
DiagnosticSeverity source_severity(MissionFinding code) {
	return code == MissionFinding::InvalidInput ? DiagnosticSeverity::Error : DiagnosticSeverity::Info;
}

} // namespace

const FindingCodeRow &finding_code(MissionFinding code) { return kFindingRows[static_cast<size_t>(code)]; }

FindingTable mission_finding_codes() { return { kFindingRows.data(), kFindingRows.size() }; }

std::vector<Diagnostic> validate_mission_file(const DocumentBase &document) {
	std::vector<Diagnostic> findings;
	const auto *mission = dynamic_cast<const MissionDocument *>(&document);
	if (!mission) return findings;
	// The source findings, each under the code its parse gave it, on the record its locator names
	// as the file was loaded, wherever that record is now (source_address; gone: the file alone).
	const std::vector<SourceIssue> &issues = mission->issues();
	const std::vector<MissionFinding> &codes = mission->issue_codes();
	for (size_t i = 0; i < issues.size(); ++i) {
		const SourceIssue &issue = issues[i];
		const MissionFinding code = i < codes.size() ? codes[i]
		                            : issue.blocks   ? MissionFinding::InvalidInput
		                                             : MissionFinding::RewriteDiffers;
		Diagnostic d = make_finding(code, source_severity(code), issue.message, document.path(), issue.field);
		d.record = issue.record;
		d.line = issue.line;
		if (!issue.locator.empty()) {
			const NodeAddress at = mission->source_address(issue.locator);
			if (at.row) {
				d.row_id = at.row;
				d.child_id = at.child;
				d.record_kind = at.kind;
			}
		}
		findings.push_back(std::move(d));
	}
	return findings;
}

} // namespace opennova::editor
