#pragma once

#include <memory>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/model/document_base.h>
#include <editor/model/finding_code_row.h>
#include <editor/model/text_document.h>

namespace opennova::editor {

// The music script type (ADR 0046 S13 D9): a music script's SCR0 bytecode (gamemus.bin,
// menumus.bin), which the game's music VM runs [orig: AudioVM_LoadScriptFile @ 0x672D20], held as
// its MUS text: read through the decompiler (formats/mus mus_decompile) and written through the
// compiler and the encoder (mus_compile, mus_encode_file) in MDEdit's layout (S23 B: every shipped
// script's text compiles back to its bytes). A file the text does not write back as it is (one of
// several chunks, one laid out otherwise) is held read only: a finding, which blocks its Save. Its
// findings are the compiler's.
std::unique_ptr<DocumentBase> make_music_script_document();
std::vector<Diagnostic> validate_music_script_file(const DocumentBase &document);
// What a music script names (DocumentType::references): each play's stream of its bank by its place (formats/mus
// mus_compile_plays), the bank the script's name made .SBF (mus_bank_name: GAMEMUS.BIN's GAMEMUS.SBF), at the play's
// name in the text (its sound_N or bound name), which no rename rewrites (a place, not a name) [orig:
// AudioVM_Op_Play @ 0x672CB0 -> AudioVM_StartSound @ 0x671FF0].
void music_script_references(const TextDocument &document, std::vector<TextReference> &out);

enum class MusicScriptFinding {
	InvalidInput,   // the script holds what its MUS text cannot carry: read only
	Unserializable, // the MUS compiler refuses the text, so it does not write
	kCount
};
const FindingCodeRow &finding_code(MusicScriptFinding code);
FindingTable music_script_finding_codes();

} // namespace opennova::editor
