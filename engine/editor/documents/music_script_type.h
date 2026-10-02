#pragma once

#include <memory>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/model/document_base.h>
#include <editor/model/finding_code_row.h>

namespace opennova::editor {

// The music script type (ADR 0046 S13 D9): a music script's SCR0 bytecode (gamemus.bin,
// menumus.bin), which the game's music VM runs [orig: AudioVM_LoadScriptFile @ 0x672D20], held as
// its MUS text: read through the decompiler (formats/mus mus_decompile) and written through the
// compiler and the encoder (mus_compile, mus_encode_file), the toolchain's canonical form. A script
// that form would not write back as it is (one with a message handler, the entry the win and lose
// music restart at, which the MUS text has no form for [orig: sub_672E50 @ 0x672e95]; one of
// several chunks; one carrying the editor-only debug information the encoder does not write) is
// held read only: a finding, which blocks its Save. Its findings are the compiler's.
std::unique_ptr<DocumentBase> make_music_script_document();
std::vector<Diagnostic> validate_music_script_file(const DocumentBase &document);

enum class MusicScriptFinding {
	InvalidInput,   // the script holds what its MUS text cannot carry: read only
	Unserializable, // the MUS compiler refuses the text, so it does not write
	kCount
};
const FindingCodeRow &finding_code(MusicScriptFinding code);
FindingTable music_script_finding_codes();

} // namespace opennova::editor
