#include "line_ends.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string_view>

#include <base/io/ascii_config.h>
#include <base/io/strutil.h>
#include <editor/assets/asset_kinds.h>
#include <editor/documents/document_types.h>
#include <editor/documents/text_types.h>
#include <editor/model/document.h>
#include <editor/model/source_state.h>
#include <editor/model/text_document.h>
#include <editor/project/project_files.h>

namespace opennova::editor {

namespace {

constexpr size_t npos = std::string_view::npos;

std::string count_of(size_t n, const char *one, const char *many) {
	return std::to_string(n) + " " + (n == 1 ? one : many);
}

// A text as the editor numbers its lines (each LF ends one) and as the game's reader cuts it (CR LF
// alone ends one): its LFs no CR comes before and the first's offset, how many lines it has as
// written, whether it holds no CR LF at all (the game reads it whole as one line), and the game's line
// that holds the first such LF: its bytes (its CR LF left out) and the lines as written it spans.
struct Reading {
	size_t lone = 0, first = npos;
	size_t lines = 0;
	bool whole = false;
	std::string line;
	size_t first_line = 0, last_line = 0;
};

size_t lfs_in(std::string_view text) { return size_t(std::count(text.begin(), text.end(), '\n')); }

Reading read_lines(std::string_view text) {
	Reading r;
	r.first = strutil::first_lone_lf(text, &r.lone);
	if (r.first == npos) return r;
	r.lines =lfs_in(text) + (text.empty() || text.back() != '\n' ? 1 : 0);
	r.whole = text.find("\r\n") == npos;
	// The game's line: from after the CR LF before the first such LF (else the start) to the next CR LF
	// after it (else the end) [orig: File_ParseASCIIFile @ 0x53D8DE].
	size_t start = r.first == 0 ? npos : text.rfind("\r\n", r.first - 1);
	start = start == npos ? 0 : start + 2;
	size_t end = text.find("\r\n", r.first);
	if (end == npos) end = text.size();
	r.line = std::string(text.substr(start, end - start));
	r.first_line = 1 + lfs_in(text.substr(0, start));
	std::string_view spans(r.line);
	if (!spans.empty() && spans.back() == '\n') spans.remove_suffix(1); // the file's last LF ends its last line
	r.last_line = r.first_line + lfs_in(spans);
	return r;
}

// What the shared ASCII walk makes of that line: skipped where it has no word (a "//" or a ';' first
// cuts it before any [orig: Terrain_TokenizeConfigLine @ 0x53CC16..0x53CC31]) or its first word starts
// with '/' [orig: File_ParseASCIIFile @ 0x53D915 / @ 0x53D91E]; only its first 1000 characters
// tokenized [orig: Terrain_TokenizeConfigLine @ 0x53CBBB]. "" for a line it reads within that.
std::string walk_words(const Reading &r) {
	io::ConfigTokens tokens;
	io::tokenize_config_line(r.line.c_str(), tokens);
	// A "//" or a ';' before any word cuts the line there: a comment, read for nothing.
	if (tokens.count == 0 && tokens.cut < std::strlen(tokens.buffer)) return ", which it skips: it begins with a comment";
	if (tokens.count == 0) return ", which it skips: it has no word";
	if (tokens.tokens[0][0] == '/') return ", which it skips: its first word starts with '/'";
	if (r.line.size() > io::kConfigMaxLineChars)
		return ", reading only its first " + std::to_string(io::kConfigMaxLineChars) + " characters";
	return std::string();
}

// The finding's words: where, what the game reads, and what it makes of it.
std::string reading_words(const Reading &r, LineReader reader) {
	std::string words =
			r.whole ? "None of the file's " + count_of(r.lines, "line", "lines") + " ends CR LF"
			        : "Line " + std::to_string(r.first_line) + " ends with an LF alone" +
			                  (r.lone > 1 ? " (" + std::to_string(r.lone) + " line ends in the file are so)"
			                              : std::string());
	words += ": the game's reader ends a line at CR LF and nowhere else, so it reads ";
	words += r.whole ? std::string("the whole file")
	                 : "lines " + std::to_string(r.first_line) + " to " + std::to_string(r.last_line);
	words += " as one line";
	if (reader == LineReader::AsciiWalk) words += walk_words(r);
	return words + ".";
}

// How many records a record document of `document`'s type reads from `bytes`; false where they do not
// read.
bool records_read(const DocumentBase &document, const std::string &bytes, const std::string &game, size_t &count) {
	const DocumentType *type = document_type_for(document.kind());
	std::unique_ptr<DocumentBase> copy = type && type->make ? type->make() : nullptr;
	Diagnostic error;
	const Document *records = copy ? records_of(*copy) : nullptr;
	if (!records ||
	    !copy->load_bytes(std::vector<uint8_t>(bytes.begin(), bytes.end()), document.path(), document.kind(), game, error))
		return false;
	count = records->rows().size();
	return !copy->blocked();
}

} // namespace

std::vector<Diagnostic> line_end_findings(const DocumentBase &document, const std::string &game) {
	std::vector<Diagnostic> findings;
	const LineReader reader = file_kind_facts(document.kind()).line_reader;
	if (reader == LineReader::None) return findings;
	const std::string file = basename_of(document.path());
	PlannedFix fix;
	fix.label = "Restore CR LF line ends";
	fix.detail = "Ends each line of " + file + " that an LF ends alone with CR LF, the line end the game's reader takes";
	if (const TextDocument *text = text_of(document)) {
		if (document.blocked()) return findings;
		const Reading r = read_lines(text->text());
		if (r.first == npos) return findings;
		std::string words = reading_words(r, reader);
		if (text->line_ends() != TextLineEnds::AsWritten) words += " Save ends every line CR LF.";
		Diagnostic d = text_finding(finding_code(CoreFinding::DocumentLineEnds), DiagnosticSeverity::Warning,
				std::move(words), *text, r.first);
		fix.detail += ", so the game reads the file's " + count_of(r.lines, "line", "lines") + " as the editor shows them.";
		fix.edits.push_back(line_ends_restore());
		d.planned.push_back(std::move(fix));
		findings.push_back(std::move(d));
		return findings;
	}
	const Document *records = records_of(document);
	const SourceState *source = records ? records->source_state() : nullptr;
	if (!source || !source->odd_lines) return findings;
	const Reading r = read_lines(*source->odd_lines);
	if (r.first == npos) return findings;
	std::string words = reading_words(r, reader);
	// What the editor reads, as the game does, against what it reads with CR LF line ends.
	size_t restored = 0;
	const bool reads = records_read(document, strutil::with_crlf_line_ends(*source->odd_lines), game, restored);
	const size_t now = records->rows().size();
	if (reads && restored != now)
		words += " Read so, the file defines " + count_of(now, "record", "records") + "; with CR LF line ends it defines " +
		         std::to_string(restored) + ".";
	Diagnostic d = make_finding(CoreFinding::DocumentLineEnds, DiagnosticSeverity::Warning, std::move(words),
			document.path());
	d.line = r.first_line;
	fix.detail += reads ? ", and reads the file again: the game then reads its " + count_of(r.lines, "line", "lines") +
	                              ", " + count_of(restored, "record", "records") + "."
	                    : ", and reads the file again.";
	if (document.dirty()) fix.detail += " The unsaved edits, made over the file as the game reads it now, give way to it.";
	fix.edits.push_back(line_ends_restore());
	d.planned.push_back(std::move(fix));
	findings.push_back(std::move(d));
	return findings;
}

} // namespace opennova::editor
