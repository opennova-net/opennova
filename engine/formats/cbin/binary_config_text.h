// The CBIN form and its ConfigFile text form, each into the other (docs/credits/cbin-re.md): a
// ConfigFile the game reads in either form [orig: ConfigFile_LoadFromFile @ 0x760a10 takes a "CBIN"
// file through its binary reader, any other through ConfigFile_ParseText @ 0x7608a0]. The text form of
// a CBIN file is what the text reader reads back as the CBIN holds it: each label a section line
// ("[TEXT]", the label upper case as the text reader takes it), each entry a line "name = value, value"
// (an integer, a float written so it reads back to the same bits, a string), CR LF after each
// (formats/configfile/config_file.h). The CBIN form keeps what the text reader reads and nothing else:
// a text with a line the reader reads none of or only part of, or an entry of more than two values, is
// none it carries whole (binary_config_text_readable).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <formats/cbin/binary_config.h>

namespace opennova::cbin {

// The text form of a CBIN file, or why it has none: what the text reader would read back as the file
// holds it. False with `why` (the first thing that does not go: a label of a character a section line
// cannot carry, an entry name a line's key cannot, a string value with a separator in it or one that
// reads as a number, a float that is no number, a value of other flags). A float is written in the
// fewest decimals the text reader's atof reads back to its very bits, with a point so it reads as a
// float.
bool binary_config_text(const BinaryConfig &config, std::string &text, std::string &why);

// A line of a text the CBIN form does not carry whole: its 1-based number and why, in words ("is outside
// any section, where the reader reads nothing").
struct ConfigTextRefusal {
	size_t line = 0;
	std::string why;
};

// Whether `text` goes in the CBIN form whole: every line one the game's ConfigFile reader reads whole (a
// blank line, a section line, an entry in a section, nothing after its values), every entry of one or
// two values [orig: ConfigFile_ParseText @ 0x7608a0]. The lines as the reader splits them, at CR LF: a
// CR or an LF alone is part of a line. False with the first other line in `refusal`.
bool binary_config_text_readable(const std::string &text, ConfigTextRefusal &refusal);

// `text` read as the game's text reader reads it, in the CBIN form under `key`: its labels, entries and
// values (an integer, a float's bits, a string), the string table `strings` first in its order, each
// other string interned after them in the order first read. A text binary_config_text_readable refuses
// is read as the reader reads it, short.
BinaryConfig binary_config_from_text(const std::string &text, const std::vector<std::string> &strings,
		uint32_t key);

} // namespace opennova::cbin
