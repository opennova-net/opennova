#pragma once

#include <memory>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/model/document_base.h>
#include <editor/model/finding_code_row.h>

namespace opennova::editor {

// The credits type (ADR 0046 S13 D9): a .kda, a marquee's DATASOURCE, which the game reads as a
// ConfigFile in either of its forms, the CBIN binary form the shipped nlist.kda is or the text form
// [orig: ConfigFile_LoadFromFile @ 0x760a10; docs/mnu/menu-re.md "Marquee credits"]. A text file is
// its own text. A CBIN file (formats/cbin/binary_config.h) is held as its text form: each label a
// section line ("[TEXT]", the label upper case as the text reader takes it), each entry a line
// "name = value, value" (an integer, a float written so it reads back to the same bits, a string),
// CR LF after each, which the game's text reader reads back as the CBIN holds it
// (runtime/menu/config_text.h); Save writes the text in the CBIN form again, under the file's own
// cipher key and with its string table first in its order, so a text left as it was writes the
// bytes it was read from. A CBIN file whose text form would not read back as it holds it (a string
// value with a space, a label of capitals, a float that is no number) is held read only: a
// finding, which blocks its Save.
std::unique_ptr<DocumentBase> make_credits_document();
std::vector<Diagnostic> validate_credits_file(const DocumentBase &document);

enum class CreditsFinding {
	InvalidInput,   // the CBIN file holds what its text form cannot carry: read only
	Unserializable, // the text does not go in the CBIN form (an entry of more than two values)
	kCount
};
const FindingCodeRow &finding_code(CreditsFinding code);
FindingTable credits_finding_codes();

} // namespace opennova::editor
