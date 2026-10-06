#pragma once

#include <cstddef>
#include <cstdint>
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

// The text type: the file is its text, but gt.ssc, which the game reads by its name decoded under a
// key chain [orig: Mission_LoadEncryptedConfig @ 0x4cdcd0]: the document shows the tag decoded and
// Save writes it back encoded, as the game's own codec does (net/novacrypto/pubcrypto.h,
// encode_key_chain). Every text kind no structured type edits yet is held by it (the deep-integration
// plan's DI-06, "every text file opens in the editor": a configuration, a text, a particle file, an
// environment, an AI profile, the HUD layout and effects, the avatars, the character attributes, the
// other defs, the score table, a NovaWorld screen; a kind the build leaves out, a mission text, is none of
// its: every kind a type edits packs, assets/asset_kinds). Where the engine has a reader of
// the kind, its validate_file is that reader's findings of the text (text_reader_findings); a kind the
// asset graph reads through the engine's reader (graph/extractors.cpp, a native kind: a particle file,
// an environment, the HUD layout, the avatars) has its open document read by that same reader, so its
// names follow its edits. A kind with no reader the editor models makes no finding.
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
// the particle reader's (particle::load_particles, the port of the effect system's), the environment's
// (env::load_env), the HUD layout's
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

// The file in the shader loader's form holding `text` (with the NUL after it every shipped shader has):
// what Save writes for a new shader.
std::vector<uint8_t> shader_file_bytes(const std::string &text);

// The fixed-function effect, which the renderer opens by this name as it starts and compiles once for each
// of its fixed-function tags [orig: HLSLEffect_InitFixedFunctionShaders @ 0x5AF790, the name @ 0x5AFA3E].
inline constexpr const char *kFixedFunctionShaderFile = "_ffp.fx";

// Those tags, as the renderer names each compile: "FF", then _ST or _MT (one texture or two), _OP, _AB or
// _AD (opaque, alpha-blended, additive), then _LUM for the self-lit [orig: @ 0x5AFAD6, sprintf "FF%s%s%s"].
const std::vector<std::string> &fixed_function_shader_tags();

// What a shader's EffectInfo annotations say [orig: HLSLEffect_LoadFromFile @ 0x5AE899..0x5AE9BC]: the tag
// the effect registers under (EffectTag) and where it is written (its offset into the text and length, 0
// for none), and whether the loader registers a TEX_UVXFORM twin of it as the tag and "#UV"
// (EffectAlt_UV) [orig: @ 0x5AEA03; HLSLEffect_LoadAllFromPFFArchive @ 0x5AFF88]. `found` is false for a
// text with no EffectInfo. Read from the text with its comments left out, as the compiler reads it; a
// preprocessor condition around the annotations is not followed.
struct ShaderEffectInfo {
	bool found = false;
	size_t info_offset = 0; // where EffectInfo is written
	std::string tag;
	size_t tag_offset = 0, tag_length = 0;
	bool alt_uv = false;
};
ShaderEffectInfo read_shader_effect_info(const std::string &text);

// The shader tags a shader file registers (DocumentType::definitions), each a Shader symbol: _ffp.fx's
// fixed-function tags (and their #UV twins where its EffectInfo asks for them); another file's EffectTag
// (and its #UV twin); none for a file whose name starts with '_', an include the archive walk skips
// [orig: HLSLEffect_LoadAllFromPFFArchive @ 0x5AFF6E].
void shader_definitions(const TextDocument &document, std::vector<TextDefinition> &out);

} // namespace opennova::editor
