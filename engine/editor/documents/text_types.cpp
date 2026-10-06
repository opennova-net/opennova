#include "text_types.h"

#include <algorithm>
#include <iterator>
#include <sstream>
#include <utility>

#include <base/io/strutil.h>
#include <editor/project/project_files.h>
#include <formats/avatars/avatars.h>
#include <formats/def/def.h>
#include <formats/env/env.h>
#include <formats/particle/parser.h>
#include <formats/scr/scr.h>
#include <formats/score/score.h>
#include <net/novacrypto/pubcrypto.h>

namespace opennova::editor {

namespace {

// The shader loader's form: "SCR", version 1, then the text and a NUL under the shaders' key
// [orig: ScriptFile_LoadAndDecrypt @ 0x5AE060: the sniff for 'S','C','R',1 @ 0x5AE0A9, the key
// 0xA55B1EED at 0x5AE0C0, one trailing NUL dropped].
constexpr uint8_t kShaderScrVersion = 1;

bool scr_header(const std::vector<uint8_t> &stored) {
	return stored.size() >= scr::SCR_HEADER_SIZE && stored[0] == 'S' && stored[1] == 'C' &&
			stored[2] == 'R';
}

// The text written back in the shader loader's form, its NUL after it where the file had one (a new
// file's as every shipped shader has it). `plain`: the file as stored is not in the form (the loader
// rejects it), a fact of the stored file, not a line of it that Save drops.
class ShaderEncoding : public TextEncoding {
public:
	ShaderEncoding(bool nul, bool plain) : nul_(nul), plain_(plain) {}
	bool plain() const { return plain_; }
	bool encode(const std::string &text, std::string &stored,
			std::vector<SourceIssue> &issues) const override {
		(void)issues;
		std::string payload = text;
		if (nul_) payload.push_back('\0');
		scr::scr_encrypt(reinterpret_cast<uint8_t *>(payload.data()), payload.size(),
				scr::SCR_KEY_SHADERS);
		stored = std::string("SCR") + char(kShaderScrVersion) + payload;
		return true;
	}

private:
	bool nul_;
	bool plain_;
};

bool decode_shader(const std::string &, const std::vector<uint8_t> &stored, std::string &text,
		std::shared_ptr<const TextEncoding> &encoding, std::vector<SourceIssue> &issues,
		std::string &error) {
	(void)issues;
	if (!scr_header(stored) || stored[3] > 2) {
		// Not in the form: the loader rejects it; the document holds it as its text, and Save
		// writes the form (the fact the encoding's: validate_file reads it).
		text.assign(stored.begin(), stored.end());
		encoding = std::make_shared<ShaderEncoding>(true, true);
		return true;
	}
	if (stored[3] != kShaderScrVersion) {
		error = "This shader is in the SCR form of version " + std::to_string(stored[3]) +
				", which the game's shader loader rejects (it reads version 1 alone).";
		return false;
	}
	std::string payload(stored.begin() + scr::SCR_HEADER_SIZE, stored.end());
	scr::scr_decrypt(reinterpret_cast<uint8_t *>(payload.data()), payload.size(),
			scr::SCR_KEY_SHADERS);
	const bool nul = !payload.empty() && payload.back() == '\0';
	if (nul) payload.pop_back();
	text = std::move(payload);
	encoding = std::make_shared<ShaderEncoding>(nul, false);
	return true;
}

// gt.ssc, the gate tag the game reads encoded by its name alone [orig: Mission_LoadEncryptedConfig @
// 0x4cdcd0: the name @ 0x7cbb50, the file read whole, at most 0x1FFF bytes, and decoded under the key
// chain @ 0x7cbb44 by NapiNP_DecodeEncryptedString @ 0x4cdd65; one that does not decode is skipped].
constexpr const char *kGateTagFile = "gt.ssc";
constexpr const char *kGateTagKeys = "jop:2:oyez";

// The gate tag written back in the form the game decodes, with what followed the encoded text in the
// file (its line end: the decode reads printable characters alone), so the tag as it was read writes
// the bytes it was read from.
class GateTagEncoding : public TextEncoding {
public:
	explicit GateTagEncoding(std::string tail) : tail_(std::move(tail)) {}
	bool encode(const std::string &text, std::string &stored, std::vector<SourceIssue> &issues) const override {
		(void)issues;
		stored = encode_key_chain(std::vector<uint8_t>(text.begin(), text.end()), kGateTagKeys) + tail_;
		return true;
	}

private:
	std::string tail_;
};

// A configuration as the game reads it: the file is its text, but gt.ssc, shown decoded (the tag the
// game reads) and written back encoded. A gt.ssc that does not decode, which the game skips, is shown
// as stored and written in the form by Save.
bool decode_config(const std::string &path, const std::vector<uint8_t> &stored, std::string &text,
		std::shared_ptr<const TextEncoding> &encoding, std::vector<SourceIssue> &issues, std::string &error) {
	(void)issues;
	(void)error;
	text.assign(stored.begin(), stored.end());
	if (!strutil::iequals(basename_of(path), kGateTagFile)) return true;
	std::vector<uint8_t> tag;
	if (!decode_key_chain(text, kGateTagKeys, tag)) {
		encoding = std::make_shared<GateTagEncoding>(std::string());
		return true;
	}
	size_t end = text.size();
	while (end > 0 && !(static_cast<unsigned char>(text[end - 1]) >= 0x20 && static_cast<unsigned char>(text[end - 1]) <= 0x7E))
		--end;
	encoding = std::make_shared<GateTagEncoding>(text.substr(end));
	text.assign(tag.begin(), tag.end());
	return true;
}

// The text type's findings (DI-06), listed: a reader's refusal is a warning, its file's names unchecked, as
// graph.unreadable said of such a file before it opened as a text; what a reader made of a line is its
// own severity's.
constexpr FindingCodeEntry<TextFinding> kTextEntries[] = {
	{ TextFinding::Unreadable, listed_code("text.unreadable") },
	{ TextFinding::Reader, listed_code("text.reader") },
};
static_assert(std::size(kTextEntries) == static_cast<size_t>(TextFinding::kCount),
		"every TextFinding has exactly one row");
static_assert(finding_entries_well_formed(kTextEntries),
		"the text type's rows follow TextFinding's order, each token its own");
constexpr auto kTextRows = finding_rows(kTextEntries, FindingGroup::Texts);
static_assert(finding_rows_well_formed(kTextRows), "every row of the table takes its group");

// A finding of a reader's at a line and a column (1-based; column 0 the line's start, line 0 no place:
// the text's start).
Diagnostic reader_finding(TextFinding code, DiagnosticSeverity severity, std::string message,
		const TextDocument &document, size_t line, size_t column) {
	size_t offset = 0;
	if (line == 0 || !document.offset_of(line, column ? column : 1, offset)) offset = 0;
	return text_finding(kTextRows[static_cast<size_t>(code)], severity, std::move(message), document, offset);
}

// A reader's message as a sentence: its first letter a capital, a full stop after it.
std::string sentence(std::string message) {
	while (!message.empty() && (message.back() == '.' || message.back() == ' ' || message.back() == '\n'))
		message.pop_back();
	if (!message.empty() && message[0] >= 'a' && message[0] <= 'z') message[0] = char(message[0] - 'a' + 'A');
	return message.empty() ? message : message + ".";
}

// A shader the loader rejects is one it does not load, as a missing one, and the game runs: listed
// (the gate follows retail, ADR 0046 S14); Save writes the form.
constexpr FindingCodeEntry<ShaderFinding> kShaderEntries[] = {
	{ ShaderFinding::Form,
			listed_code("shader.form", FindingFix::Rewrite, "in the SCR form the game's shader loader takes") },
};
static_assert(std::size(kShaderEntries) == static_cast<size_t>(ShaderFinding::kCount),
		"every ShaderFinding has exactly one row");
static_assert(finding_entries_well_formed(kShaderEntries),
		"the shader's rows follow ShaderFinding's order, each token its own");
constexpr auto kShaderRows = finding_rows(kShaderEntries, FindingGroup::Shaders);
static_assert(finding_rows_well_formed(kShaderRows), "every row of the table takes its group");

} // namespace

const std::vector<FieldSchema> &text_fields(NodeKind kind) {
	(void)kind;
	static const std::vector<FieldSchema> none;
	return none;
}

Diagnostic text_finding(const FindingCodeRow &row, DiagnosticSeverity severity, std::string message,
		const TextDocument &document, size_t offset) {
	Diagnostic finding = make_finding(row, severity, std::move(message), document.path());
	const TextSpan at = document.span_at(offset, 0);
	finding.line = at.line;
	finding.column = at.column;
	return finding;
}

std::unique_ptr<DocumentBase> make_text_document() {
	return std::make_unique<TextDocument>(decode_config);
}

std::vector<Diagnostic> text_reader_findings(const TextDocument &document) {
	std::vector<Diagnostic> findings;
	const std::string &text = document.text();
	const auto *bytes = reinterpret_cast<const uint8_t *>(text.data());
	const char *unchecked = " The editor cannot check what the file names until it reads.";
	switch (document.kind()) {
	case AssetKind::Particles: {
		particle::ParticleFile file;
		particle::ParseError error;
		if (!particle::load_particles_from_buffer(text.data(), text.size(), file, error))
			findings.push_back(reader_finding(TextFinding::Unreadable, DiagnosticSeverity::Warning,
					"The game's particle reader stops here: " + sentence(error.message) + unchecked, document,
					size_t(std::max(error.line, 0)), size_t(std::max(error.column, 0))));
		break;
	}
	case AssetKind::Environment: {
		std::istringstream input(text);
		env::Config config;
		std::string error;
		if (!env::load_env(input, config, error))
			findings.push_back(reader_finding(TextFinding::Unreadable, DiagnosticSeverity::Warning,
					"The game's environment reader does not read it: " + sentence(error) + unchecked, document, 0, 0));
		break;
	}
	case AssetKind::HudPosDefs: {
		def::DefHudPosFile file{};
		if (def::def_parse_hudpos_memory(bytes, text.size(), &file) != 0)
			findings.push_back(reader_finding(TextFinding::Unreadable, DiagnosticSeverity::Warning,
					std::string("The game's HUD layout reader does not read it.") + unchecked, document, 0, 0));
		def::def_free_hudpos(&file);
		break;
	}
	case AssetKind::AvatarDefs: {
		avatars::AvatarsFile file{};
		const bool read = avatars::avatars_parse_memory(text.data(), text.size(), &file) == 0;
		bool refused = false;
		for (size_t i = 0; i < file.diagnostics_count; ++i) {
			const avatars::AvatarDiagnostic &note = file.diagnostics[i];
			const bool error = note.severity == avatars::AVATAR_DIAG_ERROR;
			// The reader's refusal at the first error it names; its other notes listed.
			if (!read && error && !refused) {
				refused = true;
				findings.push_back(reader_finding(TextFinding::Unreadable, DiagnosticSeverity::Warning,
						"The game's avatar reader does not read it: " + sentence(note.message) + unchecked, document,
						note.line, 0));
				continue;
			}
			findings.push_back(reader_finding(TextFinding::Reader,
					error ? DiagnosticSeverity::Error : DiagnosticSeverity::Warning,
					"The game's avatar reader: " + sentence(note.message), document, note.line, 0));
		}
		if (!read && !refused)
			findings.push_back(reader_finding(TextFinding::Unreadable, DiagnosticSeverity::Warning,
					std::string("The game's avatar reader does not read it.") + unchecked, document, 0, 0));
		avatars::avatars_free(&file);
		break;
	}
	case AssetKind::Score: {
		score::File file;
		std::string error;
		if (!score::parse(bytes, text.size(), file, error))
			findings.push_back(reader_finding(TextFinding::Reader, DiagnosticSeverity::Warning,
					"The score table's reader does not read it: " + sentence(error), document, 0, 0));
		break;
	}
	default: break;
	}
	return findings;
}

std::vector<Diagnostic> validate_text_file(const DocumentBase &document) {
	const TextDocument *text = text_of(document);
	return text ? text_reader_findings(*text) : std::vector<Diagnostic>();
}

FindingTable text_finding_codes() {
	return { kTextRows.data(), kTextRows.size() };
}

const FindingCodeRow &finding_code(TextFinding code) {
	return kTextRows[static_cast<size_t>(code)];
}

std::unique_ptr<DocumentBase> make_shader_document() {
	return std::make_unique<TextDocument>(decode_shader);
}

std::vector<Diagnostic> validate_shader_file(const DocumentBase &document) {
	std::vector<Diagnostic> findings;
	const TextDocument *text = text_of(document);
	if (!text) return findings;
	// The file as stored (a save's read-back makes the encoding again from what it wrote).
	const auto *encoding = dynamic_cast<const ShaderEncoding *>(text->encoding());
	if (encoding && encoding->plain())
		findings.push_back(make_finding(ShaderFinding::Form, DiagnosticSeverity::Error,
				"The game's shader loader takes a shader in the SCR form alone and rejects this one, a "
				"plain text: Save writes it in that form.",
				document.path()));
	return findings;
}

const FindingCodeRow &finding_code(ShaderFinding code) {
	return kShaderRows[static_cast<size_t>(code)];
}

FindingTable shader_finding_codes() {
	return { kShaderRows.data(), kShaderRows.size() };
}

} // namespace opennova::editor
