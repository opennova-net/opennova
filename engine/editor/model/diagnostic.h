#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <editor/model/value.h>

namespace opennova::editor {

// One finding about a project, an asset or a field (ADR 0046 d9): the editor's
// requirements, validation, build and document layers all report through this record
// so a window, the CLI and a test read the same shape. `code` is a stable dotted
// token ("asset.name.duplicate"); `message` is plain language for the user; `asset`
// is the offending asset's project-relative path (empty for project-level findings)
// and `field` the field id inside it (empty when the whole asset is meant). A finding
// inside a document also carries the record it concerns (`record`, `row_id`,
// `child_id`, `record_kind`) so Problems can open and select it. A finding about a
// required file or a reference carries what it is about (`role`, `target`, `reference`,
// `scope`, and `loader_arg`, what the reference's loader picks the file by), so a fix and a
// filter read it rather than the message.
enum class DiagnosticSeverity { Info, Warning, Error };

struct Diagnostic {
	DiagnosticSeverity severity = DiagnosticSeverity::Error;
	std::string code;
	std::string message;
	std::string asset;
	std::string field;
	std::string record;
	size_t line = 0;
	NodeId row_id = 0, child_id = 0;
	NodeKind record_kind = 0;
	std::string role;   // a requirement's role token (a required file's finding)
	std::string target; // the file or symbol name the finding is about, as the game looks it up
	ReferenceKind reference = ReferenceKind::None; // a missing reference's kind
	std::string scope;  // a missing reference's scope (AssetGraph's: a string id's table and section)
	// What a missing reference's loader picks the file by (GraphEdge::loader_arg: a model's
	// texture row's type); -1 for any other finding.
	int32_t loader_arg = -1;
};

// The same finding, every member alike (the session's Findings concern moves only when the
// rows it composes differ).
inline bool operator==(const Diagnostic &a, const Diagnostic &b) {
	return a.severity == b.severity && a.code == b.code && a.message == b.message &&
			a.asset == b.asset && a.field == b.field && a.record == b.record && a.line == b.line &&
			a.row_id == b.row_id && a.child_id == b.child_id && a.record_kind == b.record_kind &&
			a.role == b.role && a.target == b.target && a.reference == b.reference &&
			a.scope == b.scope && a.loader_arg == b.loader_arg;
}
inline bool operator!=(const Diagnostic &a, const Diagnostic &b) { return !(a == b); }

inline Diagnostic make_diagnostic(DiagnosticSeverity severity, std::string code,
                                  std::string message, std::string asset = std::string(),
                                  std::string field = std::string()) {
	Diagnostic d;
	d.severity = severity;
	d.code = std::move(code);
	d.message = std::move(message);
	d.asset = std::move(asset);
	d.field = std::move(field);
	return d;
}

inline const char *diagnostic_severity_label(DiagnosticSeverity severity) {
	switch (severity) {
	case DiagnosticSeverity::Info: return "info";
	case DiagnosticSeverity::Warning: return "warning";
	case DiagnosticSeverity::Error: return "error";
	}
	return "error";
}

inline bool diagnostics_have_errors(const std::vector<Diagnostic> &items) {
	for (const Diagnostic &d : items) {
		if (d.severity == DiagnosticSeverity::Error) return true;
	}
	return false;
}

} // namespace opennova::editor
