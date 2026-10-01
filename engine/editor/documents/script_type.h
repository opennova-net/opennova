#pragma once

#include <memory>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/model/document_base.h>
#include <editor/model/finding_code_row.h>
#include <editor/model/text_document.h>
#include <formats/wac/program.h>

namespace opennova::editor {

// The script type (ADR 0046 S13 D9): a .wac, a text document of the file as stored, which the game
// compiles with the WAC compiler [orig: Script_Compile @ 0x4F31F0; the port is runtime/wac/compiler].
// Its findings are the compiler's reports, its references the names its operands look up.

// The compile of a script's text alone, the compiler's own pass: no catalog (the effects, the
// sound sets, the ammo and the mission text are other files', which the asset graph resolves), the
// seven default groups (a mission's own are its file's), and the file a RUN names read as empty
// (validate_file reads no other file).
wac::Program compile_script(const TextDocument &document);

std::unique_ptr<DocumentBase> make_script_document();
// What the compiler reports of the text alone, each at the token it was at: a Warning (the game
// runs the program as it compiled, its script debug overlay showing the first error [orig:
// Script_SetCompileError @ 0x4EE7C0]). A name a table the game fills from other files does not
// hold (an effect, a sound set, an ammo, a group) is no finding here: the asset graph's references
// say whether the project defines it.
std::vector<Diagnostic> validate_script_file(const DocumentBase &document);
// The names its operands look up, each at its span, as the compiler's lookups record them
// (wac::Program::catalog_lookups): an effect (FX:NAME, a particle effect), a sound set (SS:NAME),
// an ammo (AMMO:NAME, then ammo_NAME [orig: WacScript_ResolveParameter @ 0x4F2E21..0x4F2E92]) and a
// text key (TT:KEY, a string id of any table: the game reads the mission's table, then
// gametext.bin [orig: MissionText_GetStringByKeyOrGameText @ 0x51ECD0]).
void script_references(const TextDocument &document, std::vector<TextReference> &out);

enum class ScriptFinding {
	Compile, // a report of the WAC compiler
	kCount
};
const FindingCodeRow &finding_code(ScriptFinding code);
FindingTable script_finding_codes();

} // namespace opennova::editor
