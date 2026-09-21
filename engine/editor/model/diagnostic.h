#pragma once

#include <string>
#include <formats/def/def_schema.h>
#include <vector>

namespace opennova::editor {

// One finding about a project, an asset or a field (ADR 0046 d9): the editor's
// requirements, validation, build and document layers all report through this record
// so a window, the CLI and a test read the same shape. `code` is a stable dotted
// token ("asset.name.duplicate"); `message` is plain language for the user; `asset`
// is the offending asset's project-relative path (empty for project-level findings)
// and `field` the field id inside it (empty when the whole asset is meant).
enum class DiagnosticSeverity { Info, Warning, Error };

struct Diagnostic {
	DiagnosticSeverity severity = DiagnosticSeverity::Error;
	std::string code;
	std::string message;
	std::string asset;
	std::string field;
	std::string record;
	size_t line = 0;
	uint64_t row_id = 0, child_id = 0;
	def::DefRecordKind record_kind = def::DefRecordKind::Item;
};

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
