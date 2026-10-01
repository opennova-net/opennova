#include "script_type.h"

#include <iterator>
#include <string>

#include <base/io/strutil.h>
#include <editor/documents/text_types.h>
#include <formats/wac/param_type.h>
#include <runtime/wac/compiler.h>

namespace opennova::editor {

namespace {

constexpr FindingCodeEntry<ScriptFinding> kFindingEntries[] = {
	{ ScriptFinding::Compile, { "script.compile" } },
};
static_assert(std::size(kFindingEntries) == static_cast<size_t>(ScriptFinding::kCount),
		"every ScriptFinding has exactly one row");
static_assert(finding_entries_well_formed(kFindingEntries),
		"the script's rows follow ScriptFinding's order, each token its own");
constexpr auto kFindingRows = finding_rows(kFindingEntries, FindingGroup::Scripts);
static_assert(finding_rows_well_formed(kFindingRows), "every row of the table takes its group");

// The reference kind a catalog lookup is a name of; None for a lookup the graph does not read.
ReferenceKind lookup_kind(wac::ParamType kind) {
	switch (kind) {
	case wac::ParamType::Fx: return ReferenceKind::Particle;
	case wac::ParamType::SoundSet: return ReferenceKind::Sound;
	case wac::ParamType::Ammo: return ReferenceKind::Ammo;
	case wac::ParamType::TextToken: return ReferenceKind::TextId;
	default: return ReferenceKind::None;
	}
}

} // namespace

wac::Program compile_script(const TextDocument &document) {
	wac::CompileEnv env;
	env.source_names = { document.path() };
	// A RUN's file is another file: read as empty, so the run neither fails nor compiles anything
	// [orig: Script_LoadAndCompileFile @ 0x4EE660 answers whether a file loaded].
	env.load_source = [](const std::string &, std::string &text) {
		text.clear();
		return true;
	};
	return wac::compile_source(document.text(), env);
}

std::unique_ptr<DocumentBase> make_script_document() {
	return std::make_unique<TextDocument>();
}

std::vector<Diagnostic> validate_script_file(const DocumentBase &document) {
	std::vector<Diagnostic> findings;
	const TextDocument *text = text_of(document);
	if (!text) return findings;
	const wac::Program program = compile_script(*text);
	for (const wac::Diagnostic &report : program.diagnostics) {
		// A name of a table other files fill (the graph's references), and a RUN file's own reports
		// (none: a RUN's file reads as empty).
		if (report.table || report.source != 0) continue;
		const std::string said(strutil::trim_view(report.message));
		findings.push_back(text_finding(finding_code(ScriptFinding::Compile),
				DiagnosticSeverity::Warning,
				"The WAC compiler reports \"" + said +
						"\": the game runs the script as it compiled, and its script debug overlay "
						"shows the first such report.",
				*text, report.offset));
	}
	return findings;
}

void script_references(const TextDocument &document, std::vector<TextReference> &out) {
	const wac::Program program = compile_script(document);
	for (const wac::CatalogLookup &lookup : program.catalog_lookups) {
		const ReferenceKind kind = lookup_kind(lookup.kind);
		if (kind == ReferenceKind::None || lookup.source != 0 || lookup.length == 0 ||
				lookup.offset + lookup.length > document.text().size())
			continue;
		TextReference reference;
		reference.kind = kind;
		reference.value = document.text().substr(lookup.offset, lookup.length);
		// An ammo's name, then the name after "ammo_" [orig: WacScript_ResolveParameter @
		// 0x4F2E21..0x4F2E92 -> AmmoDef_LookupByName @ 0x409870, twice].
		if (kind == ReferenceKind::Ammo) reference.fallback = "ammo_" + reference.value;
		reference.span = document.span_at(lookup.offset, lookup.length);
		out.push_back(std::move(reference));
	}
}

const FindingCodeRow &finding_code(ScriptFinding code) {
	return kFindingRows[static_cast<size_t>(code)];
}

FindingTable script_finding_codes() {
	return { kFindingRows.data(), kFindingRows.size() };
}

} // namespace opennova::editor
