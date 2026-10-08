#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/model/document_base.h>

namespace opennova::editor {

// The ConfigFile pool rule: the one rule for every kind the game reads through the ConfigFile text reader
// (assets/asset_kinds.h, LineReader::ConfigFile: the credits and the character attributes). That reader
// sizes its pool of text values by their bytes (each its length and one, rounded up to the allocator's 64)
// and then clears it one byte per value [orig: ConfigFile_ParseText @ 0x7609e8], so a file holding more
// values than that pool's bytes writes zeros past it into the game's heap, which crashes the game later (a
// single-player mission start in the witness: docs/mnu/menu-re.md, the ConfigFile text reader). The count
// is the reader's own (configfile::data_strings_pool). A file over the line is document.config_overrun, an
// error at the first value past the pool, saying how many bytes past, which refuses a build (its row's
// game_refusal: the game's heap is corrupt from that load on); one in the CBIN form (a credits file
// stored so, which its type writes back so) is read by the binary reader, whose pools are cleared at their
// own sizes, and makes none. Its fix, where the kind's type says which lines change nothing its loader
// reads (DocumentType::config_idle_lines) and putting the ConfigFile's comment (';') before those of them
// that hold numbers alone (each takes values away and no byte of text) brings the file under the line:
// that edit, one step Undo takes back. Reads a text document's text as the game reads those bytes; a
// blocked document makes none.
std::vector<Diagnostic> config_overrun_findings(const DocumentBase &document);

// `text` with the ConfigFile reader's comment, ';', put before each line that starts at one of `line_starts`:
// a line opening with ';' is no entry to that reader [orig: ini_parse_section_entries @ 0x75dc04, its key
// "%[^;\n\r=]" of no character].
std::string config_commented(const std::string &text, std::vector<size_t> line_starts);

} // namespace opennova::editor
