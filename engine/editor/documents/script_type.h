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
// sound sets, the ammo and the mission text are other files', which the asset graph resolves), no
// entity registry (an SSN's net id is the mission's: "Unknown SSN" is no report here), the seven
// default groups (a mission's own are its file's), and the file a RUN names read as empty
// (validate_file reads no other file).
wac::Program compile_script(const TextDocument &document);
// How many compiles of a script's text were made so far: a validation's findings and the graph's
// references read one compile of a text, whichever asks first, kept for the texts compiled last (a
// test's count).
size_t script_compile_count();

// A script's document: a text document whose game reader ends a line at a CR (TextLineEnds::Cr), so
// Save writes every line CR LF.
std::unique_ptr<DocumentBase> make_script_document();
// What the compiler reports of the text alone, each at the token it was at: a Warning (the game
// runs the program as it compiled, its script debug overlay showing the first error [orig:
// Script_SetCompileError @ 0x4EE7C0]). A report that a table the game fills from other files lacks
// a name (Unknown FX, Unknown SOUNDSET, Unknown AMMO, Unknown Group) is no finding here: of those
// the asset graph checks an effect and an ammo (reference.missing); a sound set's name is a
// reference it does not check and a group's none, a follow-up. A line end the reader reads
// otherwise than the editor's lines (an LF alone, a CR alone) is script.line_ending, which a
// Rewrite fixes.
std::vector<Diagnostic> validate_script_file(const DocumentBase &document);
// A WAC compiler report in plain words, one sentence (ADR 0046 S15): what the report says is wrong
// where it stands, from the compiler's own legs (runtime/wac/compiler.cpp): "Missing END" is "A
// block opened on this line (an IF, a DO or a loop) has no END."; a report the table does not word
// comes back as the compiler says it. A finding's message leads with it, the compiler's words after.
std::string script_report_words(const std::string &report);
// The names its operands look up, each at its span, as the compiler's lookups record them
// (wac::Program::catalog_lookups), but a declared name's (a VAR's, a CHEAT's, an IF's: checked as
// new through the same legs, a name the script gives): an effect (FX:NAME, a particle effect), a
// sound set (SS:NAME), an ammo (AMMO:NAME, then ammo_NAME [orig: WacScript_ResolveParameter @
// 0x4F2E21..0x4F2E92]) and a text key (TT:KEY, a string id the game reads from the mission text's
// table, then gametext.bin [orig: MissionText_GetStringByKeyOrGameText @ 0x51ECD0]: for a script of a
// mission's name, where the project has that mission, its table (<stem>.bin, else medmssn.bin, the
// one or the other) then GAMETEXT.BIN, a use a rename rewrites; for one of no mission the project has
// and for game.wac and server.wac, which run with every mission, any table, and no rename).
// And the files it names (wac::Program::file_uses, ADR 0046 S14): a RUN's script (a Script, by the
// name written; the compiler's own name, the token to its first '.' plus ".wac", its second name
// where the written one does not reach it [orig: Script_LoadAndCompileFile @ 0x4EE660]) and a
// Filename slot's wave (a Wave, the string as written [orig: Wac_PlayScriptedVoiceWave @ 0x4ED610]).
void script_references(const TextDocument &document, std::vector<TextReference> &out);
// The words of the WAC language in the text, each at its span, as the compiler read them
// (wac::Program::word_uses, ADR 0046 S13 V10): its keywords, the commands of its table it emitted,
// the operands it looked a name up for (their whole token, the prefix with the name) and the files
// it names (a RUN's, a wave's), in the text's order; read from the same compile as the findings
// and the references.
void script_highlights(const TextDocument &document, std::vector<TextHighlight> &out);

enum class ScriptFinding {
	Compile,    // a report of the WAC compiler
	LineEnding, // a line end the game's reader reads otherwise: a Rewrite writes every line CR LF
	kCount
};
const FindingCodeRow &finding_code(ScriptFinding code);
FindingTable script_finding_codes();

} // namespace opennova::editor
