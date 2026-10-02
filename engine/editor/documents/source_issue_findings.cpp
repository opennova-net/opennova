#include "source_issue_findings.h"

#include <utility>

namespace opennova::editor {

void source_issue_findings(const Document &document, const FindingCodeRow &invalid,
		const FindingCodeRow &ignored, std::vector<Diagnostic> &findings,
		const std::function<void(Diagnostic &)> &place) {
	for (const SourceIssue &issue : document.issues()) {
		Diagnostic finding = make_finding(
				issue.blocks ? invalid : ignored,
				issue.blocks ? DiagnosticSeverity::Error : DiagnosticSeverity::Warning, issue.message, document.path(),
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
