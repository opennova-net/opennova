// The CBIN form's own structure (docs/credits/cbin-re.md), whatever its labels: the binary form of
// a ConfigFile, which the engine reads as it reads the text form [orig: ConfigFile_LoadFromFile @
// 0x760a10 takes a "CBIN" file through its binary reader; the writer @ 0x75e250]. Read and written
// exactly: each label (a ConfigFile section) with its entries, each entry's name and its one or two
// values (an integer, a float or a string, by the value's flags), and what the form keeps beside
// them (the cipher's key, the string table's order), so decode then encode writes the bytes read.
// cbin.h's Credits is the credits' view of the same bytes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::cbin {

struct BinaryConfig {
	// A value's flags as the writer sets them: an integer, a float's bits, a string's index.
	static constexpr uint32_t kInteger = 1;
	static constexpr uint32_t kFloat = 2;
	static constexpr uint32_t kString = 4;

	struct Value {
		uint32_t raw = 0;   // the integer, the float's bits, or the string's 1-based index
		uint32_t flags = 0; // kInteger, kFloat or kString
	};
	struct Entry {
		uint32_t name = 0;         // the name's 1-based string index
		std::vector<Value> values; // one or two (the name entry's type)
	};
	struct Label {
		uint32_t name = 0; // the label's 1-based string index
		std::vector<Entry> entries;
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
// each label's name and entry count, every label's name entries (name, type = its value count)
// each list closed by a terminator (0, 0), every entry's values in the same order, and the string
// table. False with the reason for bytes laid out otherwise (a value no entry reads, a label's
// entries not closed by its terminator, an index past the table): those would not encode back.
bool decode_binary_config(const uint8_t *data, size_t size, BinaryConfig &out, std::string &error);
// The same layout written from `config`, under its key: decode then encode gives the bytes back.
// False with the reason for an entry of no value or of more than two, or an index past the table.
bool encode_binary_config(const BinaryConfig &config, std::vector<uint8_t> &out, std::string &error);

} // namespace opennova::cbin
