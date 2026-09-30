#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include <editor/model/value.h>

namespace opennova::editor {

// One finding about a project, an asset or a field (ADR 0046 d9): the editor's
// requirements, validation, build and document layers all report through this record
// so a window, the CLI and a test read the same shape. `code` is a stable dotted
// token ("asset.name.duplicate"): the token of the finding code's row it was made from
// (session/finding_codes.h: every finding is made through make_finding, never from free
// text); `message` is plain language for the user; `asset` is the offending asset's
// project-relative path (empty for project-level findings) and `field` the field id inside
// it (empty when the whole asset is meant). A finding inside a document also carries the
// record it concerns (`record`, `row_id`, `child_id`, `record_kind`) so Problems can open
// and select it. A finding about a required file or a reference carries what it is about
// (`subject`), so a fix and a filter read it rather than the message.
enum class DiagnosticSeverity { Info, Warning, Error };

// What a finding about a file the game reads by name is about (a requirement's row, a file the
// game reported missing when it booted): the row's role token ("" for a name no row has) and the
// name the game looks the file up by.
struct RequirementSubject {
	std::string role;
	std::string target;
};

// What a finding about a reference is about: its kind, the file or symbol name as the game looks
// it up, where the name is looked up (AssetGraph's scope: a string id's table and section) and
// what its loader picks the file by (GraphEdge::loader_arg: a model's texture row's type; -1 for
// none).
struct ReferenceSubject {
	ReferenceKind kind = ReferenceKind::None;
	std::string target;
	std::string scope;
	int32_t loader_arg = -1;
};

inline bool operator==(const RequirementSubject &a, const RequirementSubject &b) {
	return a.role == b.role && a.target == b.target;
}
inline bool operator==(const ReferenceSubject &a, const ReferenceSubject &b) {
	return a.kind == b.kind && a.target == b.target && a.scope == b.scope &&
			a.loader_arg == b.loader_arg;
}

// What a finding is about beside its place: nothing more, a file the game reads by name, or a
// reference.
using FindingSubject = std::variant<std::monostate, RequirementSubject, ReferenceSubject>;

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
	FindingSubject subject;
};

// The finding's subject when it is of that kind; null otherwise.
inline const RequirementSubject *requirement_subject(const Diagnostic &d) {
	return std::get_if<RequirementSubject>(&d.subject);
}
inline const ReferenceSubject *reference_subject(const Diagnostic &d) {
	return std::get_if<ReferenceSubject>(&d.subject);
}
// The name a finding is about as the game looks it up (a required file's, a reference's); "" for a
// finding about neither.
inline const std::string &subject_target(const Diagnostic &d) {
	static const std::string none;
	if (const RequirementSubject *requirement = requirement_subject(d)) return requirement->target;
	if (const ReferenceSubject *reference = reference_subject(d)) return reference->target;
	return none;
}

// The same finding, every member alike (the session's Findings concern moves only when the
// rows it composes differ).
inline bool operator==(const Diagnostic &a, const Diagnostic &b) {
	return a.severity == b.severity && a.code == b.code && a.message == b.message &&
			a.asset == b.asset && a.field == b.field && a.record == b.record && a.line == b.line &&
			a.row_id == b.row_id && a.child_id == b.child_id && a.record_kind == b.record_kind &&
			a.subject == b.subject;
}
inline bool operator!=(const Diagnostic &a, const Diagnostic &b) { return !(a == b); }

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
