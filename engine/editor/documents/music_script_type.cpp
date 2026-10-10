#include "music_script_type.h"

#include <cstring>
#include <iterator>
#include <string>
#include <utility>

#include <editor/documents/text_types.h>
#include <editor/model/text_document.h>
#include <formats/mus/mus.h>

namespace opennova::editor {

namespace {

constexpr FindingCodeEntry<MusicScriptFinding> kFindingEntries[] = {
	{ MusicScriptFinding::InvalidInput, { "music_script.invalid_input", FindingFix::None, nullptr, true } },
	{ MusicScriptFinding::Unserializable, { "music_script.unserializable", FindingFix::None, nullptr, true } },
};
static_assert(std::size(kFindingEntries) == static_cast<size_t>(MusicScriptFinding::kCount),
		"every MusicScriptFinding has exactly one row");
static_assert(finding_entries_well_formed(kFindingEntries),
		"the music script's rows follow MusicScriptFinding's order, each token its own");
constexpr auto kFindingRows = finding_rows(kFindingEntries, FindingGroup::MusicScripts);
static_assert(finding_rows_well_formed(kFindingRows), "every row of the table takes its group");

// The MUS compiler's verdict on a text: the script's bytes in the canonical form, or where and why
// it stops (its line and column, 1-based).
struct Compiled {
	bool ok = false;
	std::string bytes;
	int line = 0, column = 0;
	std::string message;
};

Compiled compile(const std::string &text) {
	Compiled out;
	mus::MusScript script{};
	const char *message = nullptr;
	if (mus::mus_compile(text.c_str(), &script, &out.line, &out.column, &message) != 0) {
		out.message = message ? message : "compile error";
		return out;
	}
	const mus::MusScript *scripts[] = { &script };
	uint8_t *buffer = nullptr;
	size_t size = 0;
	if (mus::mus_encode_file(scripts, 1, &buffer, &size) == 0 && buffer) {
		out.bytes.assign(reinterpret_cast<const char *>(buffer), size);
		out.ok = true;
	} else {
		out.message = "the encoder does not write the compiled script";
	}
	mus::mus_free(buffer);
	mus::mus_script_free(&script);
	return out;
}

// A text written back in the script's form: compiled and encoded.
class MusicEncoding : public TextEncoding {
public:
	bool encode(const std::string &text, std::string &stored,
			std::vector<SourceIssue> &issues) const override {
		Compiled compiled = compile(text);
		if (!compiled.ok) {
			issues.push_back({true, size_t(compiled.line > 0 ? compiled.line : 0), std::string(),
					std::string(),
					"The MUS compiler stops at " + std::to_string(compiled.line) + ":" +
							std::to_string(compiled.column) + ": " + compiled.message + "."});
			return false;
		}
		stored = std::move(compiled.bytes);
		return true;
	}
};

void hold_read_only(std::vector<SourceIssue> &issues, const std::string &why) {
	issues.push_back({true, 0, std::string(), std::string(),
			"This music script " + why + ": the editor shows its MUS text read only and cannot save this file yet "
			"(the game keeps playing it as it is)."});
}

bool decode_music_script(const std::string &, const std::vector<uint8_t> &stored, std::string &text,
		std::shared_ptr<const TextEncoding> &encoding, std::vector<SourceIssue> &issues,
		std::string &error) {
	mus::MusFile file{};
	if (mus::mus_open_memory(&file, stored.data(), stored.size()) != 0 || file.header.chunk_count == 0) {
		mus::mus_close(&file);
		error = "This music script does not read: its SCR0 header or its chunk do not.";
		return false;
	}
	const mus::MusScript &script = file.scripts[0];
	const int needed = mus::mus_decompile(&script, nullptr, 0);
	if (needed < 0) {
		mus::mus_close(&file);
		error = "This music script's bytecode does not decompile.";
		return false;
	}
	text.assign(size_t(needed) + 1, '\0');
	mus::mus_decompile(&script, &text[0], text.size());
	text.resize(size_t(needed));
	encoding = std::make_shared<MusicEncoding>();
	// What the MUS text cannot carry: the script is held read only. Its text compiles to MDEdit's layout (a
	// MessageHandler its `handler`, the debug tables and the line table carried; every shipped script its own
	// bytes, formats/mus), so a file the text does not give back as it is (one written otherwise) is held so too.
	if (file.header.chunk_count != 1) {
		hold_read_only(issues, "holds " + std::to_string(file.header.chunk_count) +
				" scripts, and its MUS text the first alone");
	} else {
		const Compiled again = compile(text);
		if (!again.ok || again.bytes.size() != stored.size() ||
				std::memcmp(again.bytes.data(), stored.data(), stored.size()) != 0)
			hold_read_only(issues,
					"would not be written back as it is from its MUS text (a layout other than MDEdit's, which the "
					"compiler writes)");
	}
	mus::mus_close(&file);
	return true;
}

} // namespace

std::unique_ptr<DocumentBase> make_music_script_document() {
	return std::make_unique<TextDocument>(decode_music_script);
}

std::vector<Diagnostic> validate_music_script_file(const DocumentBase &document) {
	std::vector<Diagnostic> findings;
	const TextDocument *text = text_of(document);
	if (!text) return findings;
	for (const SourceIssue &issue : document.issues())
		findings.push_back(make_finding(MusicScriptFinding::InvalidInput, DiagnosticSeverity::Warning,
				issue.message, document.path()));
	const Compiled compiled = compile(text->text());
	if (!compiled.ok) {
		size_t offset = 0;
		const size_t line = compiled.line > 0 ? size_t(compiled.line) : 1;
		const size_t column = compiled.column > 0 ? size_t(compiled.column) : 1;
		if (!text->offset_of(line, column, offset)) offset = text->text().size();
		findings.push_back(text_finding(finding_code(MusicScriptFinding::Unserializable),
				DiagnosticSeverity::Error,
				"The MUS compiler stops here: " + compiled.message + ".", *text, offset));
	}
	return findings;
}

const FindingCodeRow &finding_code(MusicScriptFinding code) {
	return kFindingRows[static_cast<size_t>(code)];
}

FindingTable music_script_finding_codes() {
	return { kFindingRows.data(), kFindingRows.size() };
}

} // namespace opennova::editor
