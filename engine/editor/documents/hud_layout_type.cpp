#include <editor/documents/hud_layout_type.h>

#include <cstdlib>
#include <iterator>

#include <base/io/ascii_config.h>
#include <base/io/strutil.h>
#include <editor/documents/text_types.h>

namespace opennova::editor {

namespace {

constexpr FindingCodeEntry<HudLayoutFinding> kFindingEntries[] = {
	{ HudLayoutFinding::LineEnding,
			{ "hud_layout.line_ending", FindingFix::Rewrite, "with every line ending CR LF" } },
};
static_assert(std::size(kFindingEntries) == static_cast<size_t>(HudLayoutFinding::kCount),
		"every HudLayoutFinding has exactly one row");
static_assert(finding_entries_well_formed(kFindingEntries),
		"the HUD layout's rows follow HudLayoutFinding's order, each token its own");
constexpr auto kFindingRows = finding_rows(kFindingEntries, FindingGroup::HudLayouts);
static_assert(finding_rows_well_formed(kFindingRows), "every row of the table takes its group");

// A first value compared as the parser reads it: a number by its value (a HUDSTANCE's id through
// atof), anything else without case.
bool same_value(const std::string &written, const char *wanted) {
	char *end = nullptr;
	const double a = std::strtod(written.c_str(), &end);
	const bool number = !written.empty() && end && *end == '\0';
	char *wanted_end = nullptr;
	const double b = std::strtod(wanted, &wanted_end);
	if (number && wanted[0] && wanted_end && *wanted_end == '\0') return a == b;
	return strutil::iequals(written, wanted);
}

} // namespace

std::unique_ptr<DocumentBase> make_hud_layout_document() {
	// The file is its text; the game's reader ends a line at CR LF alone.
	return std::make_unique<TextDocument>(nullptr, TextLineEnds::CrLf);
}

std::vector<Diagnostic> validate_hud_layout_file(const DocumentBase &document) {
	std::vector<Diagnostic> findings;
	const TextDocument *text = text_of(document);
	if (!text || document.blocked()) return findings;
	// An LF alone, which the reader does not end a line at: Save writes it CR LF.
	size_t odd_count = 0;
	const size_t odd = text->odd_line_end(&odd_count);
	if (odd != std::string::npos)
		findings.push_back(text_finding(finding_code(HudLayoutFinding::LineEnding), DiagnosticSeverity::Warning,
				"This line ends with an LF alone" +
						(odd_count > 1 ? " (" + std::to_string(odd_count) + " line ends in the file are so)"
						               : std::string()) +
						": the game's reader ends a line at CR LF, so it reads the next line as part of this one. "
						"Save ends every line CR LF.",
				*text, odd));
	return findings;
}

const FindingCodeRow &finding_code(HudLayoutFinding code) {
	return kFindingRows[static_cast<size_t>(code)];
}

FindingTable hud_layout_finding_codes() {
	return { kFindingRows.data(), kFindingRows.size() };
}

std::vector<HudLayoutLine> hud_layout_lines(const std::string &text) {
	std::vector<HudLayoutLine> out;
	io::ConfigTokens tokens;
	size_t start = 0;
	while (start <= text.size()) {
		size_t end = text.find('\n', start);
		if (end == std::string::npos) end = text.size();
		size_t length = end - start;
		if (length > 0 && text[start + length - 1] == '\r') --length;
		// The line as the reader's string, up to its first NUL.
		const std::string line = text.substr(start, length);
		io::tokenize_config_line(line.c_str(), tokens);
		if (tokens.count > 0 && tokens.tokens[0][0] != '/') {
			HudLayoutLine row;
			row.key = tokens.tokens[0];
			if (tokens.count > 1) row.first = tokens.tokens[1];
			row.offset = start;
			row.length = length;
			out.push_back(std::move(row));
		}
		if (end == text.size()) break;
		start = end + 1;
	}
	return out;
}

const HudLayoutLine *hud_layout_line(const std::vector<HudLayoutLine> &lines, const char *key, const char *first) {
	const HudLayoutLine *found = nullptr;
	for (const HudLayoutLine &line : lines)
		if (strutil::iequals(line.key, key) && (!first || same_value(line.first, first))) found = &line;
	return found;
}

} // namespace opennova::editor
