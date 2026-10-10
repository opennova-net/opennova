#include "source_issue_findings.h"

#include <utility>

namespace opennova::editor {

void source_issue_findings(const Document &document, const FindingCodeRow &invalid,
		const FindingCodeRow &ignored, std::vector<Diagnostic> &findings,
		const std::function<void(Diagnostic &)> &place, const char *game_reads, const FindingCodeRow *stops) {
	for (const SourceIssue &issue : document.issues()) {
		const bool stopped = issue.blocks && issue.game_stops && stops;
		const FindingCodeRow &row = !issue.blocks ? ignored : stopped ? *stops : invalid;
		const std::string message =
				issue.blocks && !stopped && game_reads ? issue.message + " " + game_reads : issue.message;
		Diagnostic finding = make_finding(
				row, issue.blocks ? DiagnosticSeverity::Error : DiagnosticSeverity::Warning, message, document.path(),
				issue.field);
		finding.line = issue.line;
		finding.record = issue.record;
		const NodeAddress address =
				issue.locator.empty() ? NodeAddress() : document.source_address(issue.locator);
		if (address.row) {
			finding.row_id = address.row;
			finding.child_id = address.child;
			finding.record_kind = address.kind;
		}
		if (place)
			place(finding);
		findings.push_back(std::move(finding));
	}
}

} // namespace opennova::editor
