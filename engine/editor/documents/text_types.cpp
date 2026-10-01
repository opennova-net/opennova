#include "text_types.h"

#include <iterator>
#include <utility>

#include <formats/scr/scr.h>

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

bool decode_shader(const std::vector<uint8_t> &stored, std::string &text,
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

constexpr FindingCodeEntry<ShaderFinding> kShaderEntries[] = {
	{ ShaderFinding::Form,
			{ "shader.form", FindingFix::Rewrite, "in the SCR form the game's shader loader takes" } },
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
	return std::make_unique<TextDocument>();
}

std::vector<Diagnostic> validate_text_file(const DocumentBase &document) {
	(void)document;
	return {};
}

FindingTable text_finding_codes() {
	return {};
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
