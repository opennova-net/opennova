#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/model/document_base.h>
#include <editor/model/finding_code_row.h>
#include <editor/model/text_document.h>
#include <editor/model/value.h>

namespace opennova::editor {

// What the text types share (ADR 0046 S13 D9; the script, the music script and the credits are
// script_type.h, music_script_type.h and credits_type.h), and the two types that need nothing else:
// a shader (.fx), which the game's shader loader takes in the SCR form alone, and a text (a
// configuration: .cfg .ini .ssc .cd; a text: .txt), held as the file stores it.

// A text type's fields: none for any kind (a text holds no records).
const std::vector<FieldSchema> &text_fields(NodeKind kind);
// A finding of a text document's file at `offset` characters into its text: its line and column,
// where Problems opens the document.
Diagnostic text_finding(const FindingCodeRow &row, DiagnosticSeverity severity, std::string message,
		const TextDocument &document, size_t offset);

// The text type: the file is its text. The game reads its files through readers the editor does not
// model yet (each configuration its own), so its validate_file makes no finding and its table has
// no row.
std::unique_ptr<DocumentBase> make_text_document();
std::vector<Diagnostic> validate_text_file(const DocumentBase &document);
FindingTable text_finding_codes();

// The shader type: the game's shader loader reads a file in the SCR form alone, version 1 under
// its own key, and drops one NUL after the text [orig: ScriptFile_LoadAndDecrypt @ 0x5AE060, the
// key at 0x5AE0C0]: the document holds the text, and Save writes that form (every shipped shader
// ends its text with the NUL). A file not in the form (a plain text) is one the loader rejects: a
// finding, which a Rewrite fixes; one in the form of another version does not load.
std::unique_ptr<DocumentBase> make_shader_document();
std::vector<Diagnostic> validate_shader_file(const DocumentBase &document);

enum class ShaderFinding {
	Form, // not in the SCR form the shader loader takes
	kCount
};
const FindingCodeRow &finding_code(ShaderFinding code);
FindingTable shader_finding_codes();

} // namespace opennova::editor
