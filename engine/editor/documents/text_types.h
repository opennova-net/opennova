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
// The same at a place an engine reader names: a line and a column (1-based; column 0 the line's start,
// line 0 or a place outside the text the text's start). The text type's reader findings and the particle
// type's (documents/particle_type) are made by it.
Diagnostic text_finding_at(const FindingCodeRow &row, DiagnosticSeverity severity, std::string message,
		const TextDocument &document, size_t line, size_t column);
// An engine reader's message as a sentence: its first letter a capital, one full stop after it.
std::string reader_sentence(std::string message);

// The text type: the file is its text, but gt.ssc, which the game reads by its name decoded under a
// key chain [orig: Mission_LoadEncryptedConfig @ 0x4cdcd0]: the document shows the tag decoded and
// Save writes it back encoded, as the game's own codec does (net/novacrypto/pubcrypto.h,
// encode_key_chain). Every text kind no structured type edits yet is held by it (the deep-integration
// plan's DI-06, "every text file opens in the editor": a configuration, a text, an
// environment, an AI profile, the HUD layout and effects, the avatars, the character attributes, the
// other defs, the score table, a NovaWorld screen; a kind the build leaves out, a mission text, is none of
// its: every kind a type edits packs, assets/asset_kinds). Where the engine has a reader of
// the kind, its validate_file is that reader's findings of the text (text_reader_findings); a kind the
// asset graph reads through the engine's reader (graph/extractors.cpp, a native kind: an environment,
// the HUD layout, the avatars) has its open document read by that same reader, so its names follow its
// edits. A kind with no reader the editor models makes no finding. A particle file is no longer the text
// type's: its own type holds it (documents/particle_type, DI-14), the specific type owning its kind.
std::unique_ptr<DocumentBase> make_text_document();
std::vector<Diagnostic> validate_text_file(const DocumentBase &document);
FindingTable text_finding_codes();

// Both listed (they refuse no build).
enum class TextFinding {
	// The engine's own reader of a kind whose names the asset graph reads through it does not read the
	// file, at the place it stops where it says one: a warning, the file's names unchecked, as the
	// graph's graph.unreadable said of such a file before it opened as a text.
	Unreadable,
	// What the engine's reader makes of a line it reads past (an avatar's unknown property), at its own
	// severity; a reader's refusal where nothing the editor checks rides on it (the score table), a warning.
	Reader,
	kCount
};
const FindingCodeRow &finding_code(TextFinding code);
// The findings the engine's reader of `document`'s kind makes of its text (validate_text_file's):
// the environment's (env::load_env), the HUD layout's
// (def::def_parse_hudpos_memory), the avatars' (avatars::avatars_parse_memory, each diagnostic at its
// line) and the score table's (score::parse); none for any other kind.
std::vector<Diagnostic> text_reader_findings(const TextDocument &document);

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
