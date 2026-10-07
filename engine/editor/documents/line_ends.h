#pragma once

#include <string>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/model/document_base.h>

namespace opennova::editor {

// The line-ends rule: the one rule for every kind whose game reader ends a line at CR LF and nowhere
// else (assets/asset_kinds.h, LineReader: the defs, the AI profiles, the animation maps, the
// environments, the particle files, the credits and the character attributes). An LF that ends a
// line alone is a byte of the line to that reader, so the game reads the line it ends and the lines
// after it, up to the next CR LF, as one line [orig: File_ParseASCIIFile @ 0x53D8DE; ConfigFile_ParseText
// @ 0x7608a0], and a file of LF line ends as one line, of which the ASCII walk reads a key and at most
// 29 values from its first 1000 characters, or nothing where its first word starts with '/' (a def
// that begins with a comment defines nothing) [orig: Terrain_TokenizeConfigLine @ 0x53CB60, the clamp
// @ 0x53CBBB, the 30 cap @ 0x53CC8C; File_ParseASCIIFile @ 0x53D91E]. The editor reads the file the
// same way (the def catalogs, the sound profiles, the animation maps and the environments through the
// game's own walk), so such a file shows what the game reads: document.line_ends says so, a warning
// at the first such line, naming the lines the game reads as one, what it makes of the first of them
// and, for a record document, the records it reads against those the file holds with CR LF line ends.
// Its fix, Restore CR LF line ends, writes each such LF CR LF, one step Undo takes back: a text
// document's span of those line ends replaced, a record document's source read again
// (LineEndsRestore, model/document.h). What it reads: a text document's text (a type whose Save writes
// CR LF itself, the HUD layout's and the credits', says Save does so too); a record document's source
// (SourceState::odd_lines), the bytes its content was read from, since its writer may write the file
// otherwise (the environments' and the sound profiles' write each line CR LF). A blocked text document
// makes none; a blocked record document does, its fix reading the file again. `game` is the target
// game the record documents' second reading is made for.
std::vector<Diagnostic> line_end_findings(const DocumentBase &document, const std::string &game);

} // namespace opennova::editor
