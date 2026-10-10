// The CBIN form's own structure (docs/credits/cbin-re.md), whatever its labels: the binary form of
// a ConfigFile, which the engine reads as it reads the text form [orig: ConfigFile_LoadFromFile @
// 0x760a10 takes a "CBIN" file through its binary reader; the writer @ 0x75e250]. Read and written
// exactly: each label (a ConfigFile section) with its entries, each entry's name and its values (an
// integer, a float or a string, by the value's flags), and what the form keeps beside
// them (the cipher's key, the string table's order, each label's terminator), so decode then encode
// writes the bytes read, whatever words the game's reader takes: a null name (index 0), an entry of any number
// of values, a value of any flags, a terminator that names an entry. cbin.h's Credits is the credits' view
// of the same bytes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::configfile {
struct ConfigSection;
}

namespace opennova::cbin {

struct BinaryConfig {
	// A value's flags as the writer sets them: an integer, a float's bits, a string's index.
	static constexpr uint32_t kInteger = 1;
	static constexpr uint32_t kFloat = 2;
	static constexpr uint32_t kString = 4;

	struct Value {
		uint32_t raw = 0;   // the integer, the float's bits, or the string's 1-based index (0: none)
		uint32_t flags = 0; // kInteger, kFloat or kString as the writer sets them; any word read
	};
	struct Entry {
		uint32_t name = 0;         // the name's 1-based string index; 0 for a null name
		std::vector<Value> values; // any number (the name entry's type); the writer's one or two
	};
	struct Label {
		uint32_t name = 0; // the label's 1-based string index; 0 for a null name
		std::vector<Entry> entries;
		// The pair after a label's entries, read as one more entry, its values taken after theirs [orig:
		// ConfigFile_ParseBinary -- the element loop's count + 1 @ 0x75ea75, the data loop's @ 0x75eb0b];
		// the writer's (0, 0), a null name of no value. A label of no entry has none.
		Entry terminator;
		// Its count word is 0xFFFFFFFF: the reader takes no block of entries for it and points its section
		// at the next label's block [orig: @ 0x75ea73, the element loop skipped and the pointer set
		// @ 0x75ea6f; @ 0x75eb11; sub_75E850 @ 0x75e874]. Its entries are none.
		bool count_minus_one = false;
	};

	std::vector<Label> labels;
	std::vector<std::string> strings; // the string table, in its order (1-based by the indices)
	uint32_t xor_key = 0;             // the cipher's key [orig: the cipher loop @ 0x75e348]

	// A string by its 1-based index; null for 0 or an index past the table.
	const std::string *string_at(uint32_t index) const {
		return index >= 1 && index <= strings.size() ? &strings[index - 1] : nullptr;
	}
};

// The form's cipher over `size` bytes, its own inverse: each byte XORed with the low byte of the key
// rotated left by 7 before it, the key carried from byte to byte [orig: the cipher loop @ 0x75e348
// (rol ebx, 7; xor [blob], al); the decode loops @ 0x75e473].
void apply_cipher(uint8_t *data, size_t size, uint32_t key);

// The bytes as the form lays them out: the 20-byte header, then under the cipher the label count,
// each label's name and entry count, every label's name entries (name, type = its value count) and
// its terminator pair after them, a label of no entry (or of the count 0xFFFFFFFF) having neither
// [orig: ConfigFile_ParseBinary @ 0x75ea65; the writer @ 0x75e424..0x75e429], every entry's values,
// each label's terminator's after its entries', and the string table. Every word is taken as the
// reader takes it (D-CBIN-3): a name or a string of index 0 [orig: @ 0x75e9c4, @ 0x75ea7e,
// @ 0x75eb4e; the writer writes 0 for a null name @ 0x75e3ae, @ 0x75e45e, @ 0x75e54d], any value
// count [orig: @ 0x75eb21], any flags [orig: @ 0x75eb40], any terminator, an index past the table
// (what the accessors read of these is binary_config_sections'). False with the reason only for bytes
// that do not frame: a header that does not, a table cut short, a count past what the file holds, a
// value no entry reads. That framing is stricter than the game's reader, which deciphers only the span
// the header names and walks only the strings it counts [orig: @ 0x75e8e5..0x75e8ed, @ 0x75e94e..0x75e972]
// and reads no check that the value table meets the strings (D-CBIN-3, open).
bool decode_binary_config(const uint8_t *data, size_t size, BinaryConfig &out, std::string &error);
// The same layout written from `config`, under its key: decode then encode gives the bytes back.
// False with the reason for a label of no entry with a terminator other than the writer's (0, 0), whose
// bytes have no place for it.
bool encode_binary_config(const BinaryConfig &config, std::vector<uint8_t> &out, std::string &error);

// The sections the engine's binary reader builds of `config`, which the ConfigFile's accessors read as they
// read the text form's (formats/configfile/config_file.h) [orig: ConfigFile_ParseBinary @ 0x75e8a0, taken
// on the "CBIN" magic by ConfigFile_LoadFromFile @ 0x760aa3]: each label a section in the file's order, its
// entries those the accessors' walk reaches. The reader lays every label's entries and its terminator out
// in one block, label after label, and the walk goes from the section's first entry to the first of a null
// name [orig: Effect_GetParamValue_0 @ 0x75fa00, the tests @ 0x75fa87 / @ 0x75faaf]: a label's entries
// end at its terminator, or at a null name before it, and a terminator that names an entry is one more,
// the walk going on into the next label's block (a label counted 0xFFFFFFFF starts there); past the last
// block, the allocation's slack the parse never wrote, the entries end. As the reader's, the block is one
// that every section shares (ConfigSection::block), each section a run of it, so the storage stays linear
// however far the walks run. Every entry's value pointer is
// set, so the walk goes past an entry of no value [orig: @ 0x75eb1e]. A label's name is lowercased in the
// string table itself [orig: strlwr @ 0x75e9d4], so an entry's name or a string value of the same string
// reads lowercased too. A value keeps its flags as the type the accessors test: 1 an integer, 2 a float's
// bits, any other read as text and as the number 0 [orig: @ 0x75eb40; Effect_GetParamValue_0 @ 0x75fb2e /
// @ 0x75fb37, @ 0x75fb39..0x75fb58]; of a set holding 4 and a word in the table the text is the string
// [orig: @ 0x75eb42..0x75eb5a]; of any other flags the text reads empty, where the game takes the word for
// a string's address (D-CBIN-5). What the game faults on faults at the read that reaches it, never at the
// load: a null label (ConfigSection::null_label) at a lookup whose scan reaches it, a name past the table
// (ConfigEntry::key_faults) at a walk that reaches it, a null word or a string past the table
// (ConfigValue::text_faults) at a text read of it; a numeric read never touches the word. False (out
// empty) for a label past the table alone, a stray pointer the parse itself lowercases through.
bool binary_config_sections(const BinaryConfig &config, std::vector<configfile::ConfigSection> &out);

} // namespace opennova::cbin
