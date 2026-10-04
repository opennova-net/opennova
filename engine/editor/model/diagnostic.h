#pragma once

#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <editor/model/finding_code_row.h>
#include <editor/model/value.h>

namespace opennova::editor {

// One finding about a project, an asset or a field (ADR 0046 d9): the editor's requirements,
// validation, build and document layers all report through this record so a window, the CLI and a
// test read the same shape. A finding is made from a row of the finding codes' tables and keeps it
// (`row()`: model/finding_code_row.h; make_finding below is the only way to set it), its `code()`
// the row's stable dotted token ("asset.name.duplicate"), the wire's `code`. `message` is plain
// language for the user; `asset` is the offending asset's project-relative path (empty for
// project-level findings) and `field` the field id inside it (empty when the whole asset is
// meant). A finding inside a document also carries the record it concerns (`record`, `row_id`,
// `child_id`, `record_kind`) so Problems can open and select it, and a finding inside a text
// document the place in it (`line`, `column`: 1-based, the character a compiler reported at; ADR
// 0046 S13 D9), which Problems opens the document at. A finding about a required file
// or a reference carries what it is about (`subject`), so a fix and a filter read it rather than
// the message.
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

struct Diagnostic;

// A finding of a row's code, at a place (the file's project-relative path, the field). The row is
// a table's (finding_code, a document type's own finding_code overloads), which the finding keeps.
inline Diagnostic make_finding(const FindingCodeRow &row, DiagnosticSeverity severity,
		std::string message, std::string asset = std::string(), std::string field = std::string());

struct Diagnostic {
	DiagnosticSeverity severity = DiagnosticSeverity::Error;
	std::string message;
	std::string asset;
	std::string field;
	std::string record;
	size_t line = 0;
	size_t column = 0; // in a text document, the character on `line` (1-based); 0 for none
	NodeId row_id = 0, child_id = 0;
	NodeKind record_kind = 0;
	// The record as itself (Document::record_identity: its kind and own name, else a digest of what it
	// holds), what the game's own data's fold keys the finding on; "" where it names no record.
	std::string record_key;
	// The record in its type's own words where they are not its name (a menu's action as what it does),
	// cached where the finding is made with its document or its edge at hand, so a Problems row of a
	// closed file names it in the same words as an open one's (the plain-words lane); "" where its name
	// says it.
	std::string record_title;
	FindingSubject subject;

	// The row the finding was made from; null for a Diagnostic no finding was made into (an error
	// left as it was because nothing failed).
	const FindingCodeRow *row() const { return row_; }
	// Its code, the row's token ("" with no row).
	const std::string &code() const { return code_; }

private:
	friend Diagnostic make_finding(const FindingCodeRow &row, DiagnosticSeverity severity,
			std::string message, std::string asset, std::string field);

	const FindingCodeRow *row_ = nullptr;
	std::string code_;
};

inline Diagnostic make_finding(const FindingCodeRow &row, DiagnosticSeverity severity,
		std::string message, std::string asset, std::string field) {
	Diagnostic d;
	d.row_ = &row;
	d.code_ = row.token ? row.token : "";
	d.severity = severity;
	d.message = std::move(message);
	d.asset = std::move(asset);
	d.field = std::move(field);
	return d;
}

// A finding of a table's code, by its enumerator: CoreFinding's, or a document type's own enum
// (whose finding_code overload, declared beside it in the editor's namespace, names its row). The
// codes of other namespaces a type keys rows by (the runtime's compiler notes, the menu type's; the
// stylesheet reader's, the stylesheet type's) take the row form:
// make_finding(finding_code(note), ...).
template <typename Code, typename = std::enable_if_t<std::is_enum<Code>::value>>
Diagnostic make_finding(Code code, DiagnosticSeverity severity, std::string message,
		std::string asset = std::string(), std::string field = std::string()) {
	return make_finding(finding_code(code), severity, std::move(message), std::move(asset),
			std::move(field));
}

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
	return a.severity == b.severity && a.row() == b.row() && a.message == b.message &&
			a.asset == b.asset && a.field == b.field && a.record == b.record && a.line == b.line &&
			a.column == b.column && a.row_id == b.row_id && a.child_id == b.child_id && a.record_kind == b.record_kind &&
			a.record_key == b.record_key && a.record_title == b.record_title && a.subject == b.subject;
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
