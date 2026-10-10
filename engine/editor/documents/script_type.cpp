#include "script_type.h"

#include <algorithm>
#include <deque>
#include <iterator>
#include <mutex>
#include <string>

#include <base/io/strutil.h>
#include <editor/documents/text_types.h>
#include <formats/mission/mission_params.h>
#include <formats/wac/param_type.h>
#include <runtime/hud/game_text_lookup.h>
#include <runtime/mission/mission_sidecars.h>
#include <runtime/wac/compiler.h>

namespace opennova::editor {

namespace {

constexpr FindingCodeEntry<ScriptFinding> kFindingEntries[] = {
	{ ScriptFinding::Compile, { "script.compile" } },
	{ ScriptFinding::LineEnding, { "script.line_ending", FindingFix::Rewrite, "with every line ending CR LF" } },
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

size_t g_compiles = 0;

// Where a report is in the text: on the line the compiler counted (its CRs [orig: Script_Compile @
// 0x4F32E0..0x4F3321], the line the game's script overlay shows), at the token it was at when that
// token is on the line (its column), else at the line's start (a "Missing END" names the line its
// block opened on, at the end of the file).
size_t report_offset(const std::string &text, int line, size_t token) {
	size_t start = 0;
	for (int crs = 1; crs < line; ++crs) {
		const size_t cr = text.find('\r', start);
		if (cr == std::string::npos) return std::min(token, text.size());
		start = cr + 1;
	}
	// A CR LF's LF is the line end's (a blank to the compiler): the line starts after it.
	if (start > 0 && start < text.size() && text[start] == '\n' && text[start - 1] == '\r') ++start;
	const size_t end = std::min(text.find('\r', start), text.size());
	return token >= start && token <= end ? token : start;
}

// A script's compile, kept for the texts compiled last (at most 8 MiB of them, the oldest let go
// first): a validation's findings and the graph's references, which each read the file (an open
// document, or a closed file each loads), read one compile of the same text.
std::shared_ptr<const wac::Program> compiled(const TextDocument &document) {
	struct Kept {
		std::string path, text;
		std::shared_ptr<const wac::Program> program;
	};
	static std::mutex mutex;
	static std::deque<Kept> kept; // the most recent last
	static size_t kept_bytes = 0;
	constexpr size_t kKeptBytes = size_t(8) << 20;
	{
		const std::lock_guard<std::mutex> lock(mutex);
		for (const Kept &entry : kept)
			if (entry.path == document.path() && entry.text == document.text()) return entry.program;
	}
	auto program = std::make_shared<const wac::Program>(compile_script(document));
	const std::lock_guard<std::mutex> lock(mutex);
	++g_compiles;
	kept.push_back({document.path(), document.text(), program});
	kept_bytes += document.text().size();
	while (kept_bytes > kKeptBytes && kept.size() > 1) {
		kept_bytes -= kept.front().text.size();
		kept.pop_front();
	}
	return program;
}

} // namespace

size_t script_compile_count() {
	return g_compiles;
}

std::string script_report_words(const std::string &report) {
	// Each report by the leg that makes it (runtime/wac/compiler.cpp, Script_Compile @ 0x4F31F0 and
	// WacScript_ResolveParameter @ 0x4F2920).
	struct Row {
		const char *report, *words;
	};
	static constexpr Row kRows[] = {
		{"Missing END", "A block opened on this line (an IF, a DOSEQ, a DORND or a loop) has no END."},
		{"Unexpected END", "This END closes no block: none is open here."},
		{"END inside IF", "An END among an IF's conditions, before its THEN."},
		{"Unexpected ELSE", "An ELSE among an IF's conditions, before its THEN."},
		{"Unexpected ELSEIF", "An ELSEIF among an IF's conditions, before its THEN."},
		{"ELSE without THEN/ENTER/LEAVE", "This ELSE follows no THEN, ENTER or LEAVE block."},
		{"ELSEIF without THEN/ENTER/LEAVE", "This ELSEIF follows no THEN, ENTER or LEAVE block."},
		{"Unexpected NEXT", "A NEXT among an IF's conditions."},
		{"NEXT without DO", "This NEXT is in no DOSEQ or DORND block."},
		{"Unexpected IF", "An IF among another IF's conditions: a THEN is missing before it."},
		{"Unexpected GLOOP", "A GLOOP among an IF's conditions."},
		{"Unexpected PLOOP", "A PLOOP among an IF's conditions."},
		{"No LOOP Nesting!", "A loop inside another loop: the compiler nests none."},
		{"[ifname] without IF", "A name in brackets with no IF before it to name."},
		{"If already named", "This IF has a name already."},
		{"IF Name already used", "Another IF, a variable or a word of the language has this name already."},
		{"Variable Name already used", "A variable, an IF or a word of the language has this name already."},
		{"Over Variable Buffersize", "Too many variables are declared: the compiler's table of them is full."},
		{"Wrong Parameter", "This value is not of the kind the command's parameter takes."},
		{"Variable not set", "An assignment with no value after its =."},
		{"Unexpected =", "An = after an operator that waits for its value."},
		{"Open Paren", "A ( is left open at the end of the line."},
		{"Unexpected )", "This ) closes no (."},
		{"Paren nesting too deep", "Parentheses nest deeper than the compiler holds."},
		{"Auto Paren nesting too deep", "The operators nest deeper than the compiler holds."},
		{"Unexpected NOT", "Two NOTs in a row."},
		{"Can't run files inside blocks", "A RUN inside a block: a file runs only at the top level."},
		{"A run file can't run more files", "A RUN inside a file another RUN brought in."},
		{"Unable to run file", "The file this RUN names could not be read."},
		{"Out of IF space", "Too many IFs: the compiler's table of them is full."},
		{"Out of DO space", "Too many DOSEQ and DORND blocks: the compiler's table of them is full."},
		{"Out of num space", "Too many different numbers: the compiler's table of them is full."},
		{"Out of string space", "Too much text: the compiler's table of strings is full."},
		{"Over Compile Buffersize", "The script is longer than the compiler's buffer holds."},
		{"V# too big", "A mission variable past V255: the compiler reads V255."},
		{"G# too big", "A global variable past G255: the compiler reads G255."},
		{"Unknown Group", "No script group has this name: the compiler reads group 0."},
		{"Unknown FX", "No particle effect has this name."},
		{"Unknown SOUNDSET", "No sound set has this name."},
		{"Unknown AMMO", "No ammo has this name, nor ammo_ with it."},
		{"Unknown FACE", "No facial expression has this name."},
		{"Unknown ANIM", "No animation slot has this name."},
		{"Unknown SSN", "No entity of the mission has this SSN."},
	};
	const std::string said(strutil::trim_view(report));
	for (const Row &row : kRows)
		if (said == row.report) return row.words;
	// "Unknown 'TOKEN'": a word that is no command, keyword, variable or name.
	if (said.rfind("Unknown '", 0) == 0 && said.size() > 10)
		return "'" + said.substr(9, said.size() - 10) + "' is no command, keyword, variable or name the compiler knows.";
	return said;
}

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
	return std::make_unique<TextDocument>(nullptr, TextLineEnds::Cr);
}

std::vector<Diagnostic> validate_script_file(const DocumentBase &document) {
	std::vector<Diagnostic> findings;
	const TextDocument *text = text_of(document);
	if (!text) return findings;
	const std::shared_ptr<const wac::Program> program = compiled(*text);
	for (const wac::Diagnostic &report : program->diagnostics) {
		// A name of a table other files fill (the graph's references), and a RUN file's own reports
		// (none: a RUN's file reads as empty).
		if (report.table || report.source != 0) continue;
		const std::string said(strutil::trim_view(report.message));
		findings.push_back(text_finding(finding_code(ScriptFinding::Compile),
				DiagnosticSeverity::Warning,
				script_report_words(said) + " (The WAC compiler: \"" + said +
						"\". The game runs the script as it compiled; its script debug overlay "
						"shows the first such report.)",
				*text, report_offset(text->text(), report.line, report.offset)));
	}
	// A line end the reader reads otherwise than the editor's lines: it ends a line at a CR and reads
	// an LF as a blank [orig: Script_Compile @ 0x4F32E0..0x4F3321, a comment running to the next CR @
	// 0x4F54BA..0x4F54D9].
	size_t odd_count = 0;
	const size_t odd = text->odd_line_end(&odd_count);
	if (odd != std::string::npos) {
		const std::string more =
				odd_count > 1 ? " (" + std::to_string(odd_count) + " line ends in the file are so)" : "";
		findings.push_back(text_finding(finding_code(ScriptFinding::LineEnding), DiagnosticSeverity::Warning,
				text->text()[odd] == '\n'
						? "This line ends with an LF alone" + more +
								": the game's script reader ends a line at a CR and reads an LF as a blank, "
								"so it reads the next line as part of this one (a comment here runs on into "
								"it). Save ends every line CR LF."
						: "A CR alone sits here" + more +
								": the game's script reader ends a line at it, where the editor shows one "
								"line. Save ends every line CR LF.",
				*text, odd));
	}
	return findings;
}

void script_references(const TextDocument &document, std::vector<TextReference> &out) {
	const std::shared_ptr<const wac::Program> program = compiled(document);
	const std::string &text = document.text();
	// A text key reads the mission text's table, then gametext.bin [orig:
	// MissionText_GetStringByKeyOrGameText @ 0x51ECD0: the loaded mission table's entry, else
	// gametext's; "" with no mission table]. The mission text is the table of the mission's own name,
	// else medmssn.bin, the one or the other [orig: TextResource_LoadMissionTextBin @ 0x51ed90]: a
	// script of a mission's name (compiled after game.wac and server.wac [orig: WacScript_InitAndLoad @
	// 0x4F91F0]) reads that mission's, where the project has the mission (its owner, <stem>.bms); one of
	// no mission's name (a RUN's file, compiled into whichever mission runs it) and game.wac and
	// server.wac, which run with every mission, read the table of whichever plays, so their keys
	// resolve in any table and no rename rewrites them.
	const std::string stem = strutil::to_upper(mission::mission_base_name(document.path()));
	const bool missions_own = stem != "GAME" && stem != "SERVER";
	for (const wac::CatalogLookup &lookup : program->catalog_lookups) {
		const ReferenceKind kind = lookup_kind(lookup.kind);
		if (kind == ReferenceKind::None || lookup.source != 0 || lookup.declaration ||
				lookup.length == 0 || lookup.offset + lookup.length > text.size())
			continue;
		TextReference reference;
		reference.kind = kind;
		reference.value = text.substr(lookup.offset, lookup.length);
		// An ammo's name, then the name after "ammo_" [orig: WacScript_ResolveParameter @
		// 0x4F2E21..0x4F2E92 -> AmmoDef_LookupByName @ 0x409870, twice].
		if (kind == ReferenceKind::Ammo) reference.fallback = "ammo_" + reference.value;
		if (kind == ReferenceKind::TextId) {
			if (missions_own) {
				// The by-name table's text row (<stem>.bin, else medmssn.bin), then the boot's gametext.bin.
				const mission::Sidecar &table = *mission::sidecar_for_role("text");
				reference.scope = strutil::to_upper(mission::sidecar_name(stem, table));
				reference.scope_alternate = strutil::to_upper(table.fallback);
				reference.scope_owner = stem + ".BMS";
				reference.scopes_after = { strutil::to_upper(hud::kGameTextTable) };
			} else {
				reference.rewritable = false;
			}
		}
		reference.span = document.span_at(lookup.offset, lookup.length);
		out.push_back(std::move(reference));
	}
	// The files the script names (wac::FileUse): a RUN's script by the name written, which the
	// kind's extension reaches as the compiler's rule does (the token to its first '.', then ".wac")
	// but for a name written with another extension, reached through the compiler's name; a wave by
	// its string.
	for (const wac::FileUse &use : program->file_uses) {
		if (use.source != 0 || use.length == 0 || use.offset + use.length > text.size()) continue;
		TextReference reference;
		reference.kind = use.kind == wac::FileUse::Kind::Run ? ReferenceKind::Script : ReferenceKind::Wave;
		reference.value = text.substr(use.offset, use.length);
		if (use.kind == wac::FileUse::Kind::Run && !strutil::iequals(use.name, reference.value) &&
				!strutil::iequals(use.name, reference.value + ".wac"))
			reference.fallback = use.name;
		reference.span = document.span_at(use.offset, use.length);
		out.push_back(std::move(reference));
	}
	// The mission records its literal operands name (wac::OperandUse): an entity by its SSN and an area by its zone
	// id, in the mission of the script's name (<stem>.BMS), only beside that mission (GraphEdge::needs); the game's
	// lookups read the playing mission's [orig: EntityPool_FindByNetId @ 0x4F0A20; WacCmd_SsnArea @ 0x4F1087, the
	// area table by zone id]. Never renamed: a number, which a renumbering of the mission's does not follow into a
	// script. The player's SSN (10000) names no record. A script of no mission's name (a RUN's file, game.wac,
	// server.wac) runs with whichever mission plays, so names none to check; a waypoint path's number is a record
	// of its mission's file by its place there, which a script's reference cannot scope (none either); a group is
	// one of the script groups, the game's own seven and an XML file's [orig: XML_ParseGroupMember @ 0x4CD6F0], no
	// mission's record.
	if (missions_own) {
		for (const wac::OperandUse &use : program->operand_uses) {
			if (use.source != 0 || use.length == 0 || use.offset + use.length > text.size()) continue;
			ReferenceKind kind = ReferenceKind::None;
			if (use.kind == wac::ParamType::Ssn && use.value != mission::kPlayerSsn)
				kind = ReferenceKind::MissionEntity;
			else if (use.kind == wac::ParamType::Area)
				kind = ReferenceKind::MissionZone;
			if (kind == ReferenceKind::None) continue;
			TextReference reference;
			reference.kind = kind;
			reference.value = std::to_string(use.value);
			reference.scope = stem + ".BMS";
			reference.needs = stem + ".BMS";
			reference.field = "operand";
			reference.rewritable = false;
			reference.span = document.span_at(use.offset, use.length);
			out.push_back(std::move(reference));
		}
	}
}

void script_highlights(const TextDocument &document, std::vector<TextHighlight> &out) {
	const std::shared_ptr<const wac::Program> program = compiled(document);
	const size_t first = out.size();
	for (const wac::WordUse &word : program->word_uses) {
		if (word.source != 0 || word.length == 0 || word.offset + word.length > document.text().size()) continue;
		TextHighlight highlight;
		switch (word.kind) {
		case wac::WordUse::Kind::Keyword: highlight.kind = TextHighlightKind::Keyword; break;
		case wac::WordUse::Kind::Command: highlight.kind = TextHighlightKind::Command; break;
		case wac::WordUse::Kind::Operand: highlight.kind = TextHighlightKind::Operand; break;
		}
		highlight.span = document.span_at(word.offset, word.length);
		out.push_back(highlight);
	}
	// In the text's order (the compiler reads it forward, but a word it took again after a lookahead
	// is noted where it took it).
	std::stable_sort(out.begin() + std::ptrdiff_t(first), out.end(), [](const TextHighlight &a, const TextHighlight &b) {
		return a.span.line != b.span.line ? a.span.line < b.span.line : a.span.column < b.span.column;
	});
}

const FindingCodeRow &finding_code(ScriptFinding code) {
	return kFindingRows[static_cast<size_t>(code)];
}

FindingTable script_finding_codes() {
	return { kFindingRows.data(), kFindingRows.size() };
}

} // namespace opennova::editor
