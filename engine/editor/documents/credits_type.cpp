#include "credits_type.h"

#include <cstring>
#include <iterator>
#include <string>
#include <utility>

#include <base/io/strutil.h>
#include <editor/documents/text_types.h>
#include <editor/model/text_document.h>
#include <formats/cbin/binary_config.h>
#include <formats/cbin/binary_config_text.h>
#include <formats/cbin/cbin.h>
#include <formats/configfile/config_file.h>

namespace opennova::editor {

namespace {

using cbin::BinaryConfig;

constexpr FindingCodeEntry<CreditsFinding> kFindingEntries[] = {
	{ CreditsFinding::InvalidInput, { "credits.invalid_input", FindingFix::None, nullptr, true } },
	{ CreditsFinding::Unserializable, { "credits.unserializable", FindingFix::None, nullptr, true } },
};
static_assert(std::size(kFindingEntries) == static_cast<size_t>(CreditsFinding::kCount),
		"every CreditsFinding has exactly one row");
static_assert(finding_entries_well_formed(kFindingEntries),
		"the credits' rows follow CreditsFinding's order, each token its own");
constexpr auto kFindingRows = finding_rows(kFindingEntries, FindingGroup::Credits);
static_assert(finding_rows_well_formed(kFindingRows), "every row of the table takes its group");

// The text read as the game's text reader reads it, in the CBIN form under the file's key, its
// string table first in its order; false with the issue for a text that does not go in it.
class CreditsEncoding : public TextEncoding {
public:
	CreditsEncoding(std::vector<std::string> strings, uint32_t key) :
			strings_(std::move(strings)), key_(key) {}

	bool encode(const std::string &text, std::string &stored,
			std::vector<SourceIssue> &issues) const override {
		// What the reader does not read whole is no part of the form: refused, never written short.
		if (!credits_text_readable(text, issues)) return false;
		const BinaryConfig config = cbin::binary_config_from_text(text, strings_, key_);
		std::vector<uint8_t> bytes;
		std::string error;
		if (!cbin::encode_binary_config(config, bytes, error)) {
			issues.push_back({true, 0, std::string(), std::string(), error});
			return false;
		}
		stored.assign(bytes.begin(), bytes.end());
		return true;
	}

private:
	std::vector<std::string> strings_;
	uint32_t key_;
};

bool decode_credits(const std::string &, const std::vector<uint8_t> &stored, std::string &text,
		std::shared_ptr<const TextEncoding> &encoding, std::vector<SourceIssue> &issues,
		std::string &error) {
	if (!cbin::is_cbin(stored.data(), stored.size())) {
		text.assign(stored.begin(), stored.end()); // the text form: the file is its text
		return true;
	}
	BinaryConfig config;
	if (!cbin::decode_binary_config(stored.data(), stored.size(), config, error)) return false;
	auto kept = std::make_shared<CreditsEncoding>(config.strings, config.xor_key);
	std::string why;
	if (!cbin::binary_config_text(config, text, why)) {
		issues.push_back({true, 0, std::string(), std::string(),
				"This credits file's CBIN form holds what its text form cannot carry (" + why +
						"): the editor shows the rest read only and cannot save this file yet (the game keeps "
						"showing it as it is)."});
		encoding = std::move(kept);
		return true;
	}
	// The text written back is the file it was read from, or the file is held read only.
	std::string again;
	std::vector<SourceIssue> refused;
	if (!kept->encode(text, again, refused) ||
			again.size() != stored.size() ||
			std::memcmp(again.data(), stored.data(), stored.size()) != 0)
		issues.push_back({true, 0, std::string(), std::string(),
				"This credits file's CBIN form would not be written back as it is from its text "
				"(its string table holds a text twice, or what reads back differs): the editor "
				"shows it read only and cannot save this file yet (the game keeps showing it as it is)."});
	encoding = std::move(kept);
	return true;
}

} // namespace

bool credits_text_readable(const std::string &text, std::vector<SourceIssue> &issues) {
	cbin::ConfigTextRefusal refusal;
	if (cbin::binary_config_text_readable(text, refusal)) return true;
	issues.push_back({true, refusal.line, std::string(), std::string(),
			"Line " + std::to_string(refusal.line) + " " + refusal.why +
					": the CBIN form keeps what the game's reader reads, so Save is refused until the "
					"line reads whole."});
	return false;
}

// [orig: CMarqueeWnd_LoadCreditsFromIni @ 0x65c5a0, its [TEXT] walk: each TEXT entry's first value cut to
// 127 bytes (String_CopyN, 0x80); `~F` its '|' tokens after the code (strtok "|" @ 0x7E1788), the third the
// texture; `~I` the rest the texture; any other line but `<CR>` a text line, its second value the font cut to
// 31 bytes (0x20)]
void credits_references(const TextDocument &document, std::vector<TextReference> &out) {
	const std::string &text = document.text();
	std::vector<configfile::ConfigSection> sections =
			configfile::parse_config_text(reinterpret_cast<const uint8_t *>(text.data()), text.size());
	configfile::ConfigSection *lines = configfile::find_config_section(sections, "TEXT");
	if (lines == nullptr) return;
	const auto add = [&](ReferenceKind kind, const configfile::ConfigValue &value, size_t at, std::string name) {
		if (name.empty()) return;
		TextReference reference;
		reference.kind = kind;
		reference.span = document.span_at(value.offset + at, name.size());
		reference.value = std::move(name);
		out.push_back(std::move(reference));
	};
	std::string line;
	bool more = configfile::read_config_value(*lines, "TEXT", 1, &line, nullptr, nullptr);
	while (more) {
		const configfile::ConfigEntry &entry = lines->entries[lines->current];
		// A value the reader takes as a number is read back printed, no name its text spells.
		const bool spelled = entry.values[0].type == 4;
		if (line.size() >= 0x80) line.resize(0x80 - 1);
		const char code = line.size() > 1 && line[0] == '~' ? line[1] : '\0';
		if (strutil::iequals(line, "<CR>")) {
		} else if (!line.empty() && line[0] == '~') {
			if (spelled && (code == 'F' || code == 'f')) {
				// x|y|texture: the third token.
				size_t p = 2, token = 0;
				while (p < line.size()) {
					while (p < line.size() && line[p] == '|') ++p;
					const size_t start = p;
					while (p < line.size() && line[p] != '|') ++p;
					if (p > start && ++token == 3) {
						add(ReferenceKind::MenuTexture, entry.values[0], start, line.substr(start, p - start));
						break;
					}
				}
			} else if (spelled && (code == 'I' || code == 'i')) {
				add(ReferenceKind::MenuTexture, entry.values[0], 2, line.substr(2));
			}
		} else {
			std::string font;
			if (configfile::read_current_config_value(*lines, "TEXT", 2, &font, nullptr, nullptr) &&
					entry.values[1].type == 4) {
				if (font.size() >= 0x20) font.resize(0x20 - 1);
				add(ReferenceKind::Font, entry.values[1], 0, std::move(font));
			}
		}
		more = configfile::read_config_value(*lines, "TEXT", 1, &line, nullptr, nullptr);
	}
}

std::unique_ptr<DocumentBase> make_credits_document() {
	return std::make_unique<TextDocument>(decode_credits, TextLineEnds::CrLf);
}

std::vector<Diagnostic> validate_credits_file(const DocumentBase &document) {
	std::vector<Diagnostic> findings;
	const TextDocument *text = text_of(document);
	if (!text) return findings;
	for (const SourceIssue &issue : document.issues())
		findings.push_back(make_finding(CreditsFinding::InvalidInput, DiagnosticSeverity::Warning,
				issue.message, document.path()));
	if (document.blocked()) return findings;
	// An LF alone, which the reader does not end a line at, is the line-ends rule's (documents/line_ends.h).
	const SerializeResult written = document.serialize();
	for (const SourceIssue &issue : written.issues) {
		size_t offset = 0;
		if (issue.line && text->offset_of(issue.line, 1, offset))
			findings.push_back(text_finding(finding_code(CreditsFinding::Unserializable), DiagnosticSeverity::Error,
					issue.message, *text, offset));
		else
			findings.push_back(make_finding(CreditsFinding::Unserializable, DiagnosticSeverity::Error,
					issue.message, document.path()));
	}
	return findings;
}

const FindingCodeRow &finding_code(CreditsFinding code) {
	return kFindingRows[static_cast<size_t>(code)];
}

FindingTable credits_finding_codes() {
	return { kFindingRows.data(), kFindingRows.size() };
}

} // namespace opennova::editor
