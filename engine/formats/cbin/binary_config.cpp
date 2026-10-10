// The CBIN form read and written exactly (binary_config.h). The layout is the one cbin.cpp's
// Credits reader walks and its writer lays out [orig: the writer @ 0x75e250: magic @ 0x75e311,
// the 20-byte header, the cipher loop @ 0x75e348], read here whatever the labels.
#include <formats/cbin/binary_config.h>

#include <base/io/le.h>
#include <formats/cbin/cbin.h>
#include <formats/configfile/config_file.h>

#include <cstring>
#include <memory>

namespace opennova::cbin {

namespace {

constexpr size_t kHeaderSize = 20;

bool fail(std::string &error, const char *why) {
	error = why;
	return false;
}

uint32_t rol32(uint32_t value, unsigned count) {
	count &= 31;
	return count ? (value << count) | (value >> (32 - count)) : value;
}

} // namespace

void apply_cipher(uint8_t *data, size_t size, uint32_t key) {
	for (size_t i = 0; i < size; ++i) {
		key = rol32(key, 7);
		data[i] ^= static_cast<uint8_t>(key & 0xFF);
	}
}

bool decode_binary_config(const uint8_t *data, size_t size, BinaryConfig &out, std::string &error) {
	out = BinaryConfig();
	if (!data || size < kHeaderSize || io::read_u32_le(data) != kMagic)
		return fail(error, "Not a CBIN file.");
	const uint32_t string_offset = io::read_u32_le(data + 0x04);
	const uint32_t blob_length = io::read_u32_le(data + 0x08);
	const uint32_t string_count = io::read_u32_le(data + 0x0C);
	out.xor_key = io::read_u32_le(data + 0x10);
	if (string_offset < kHeaderSize || string_offset > size || blob_length != size - string_offset)
		return fail(error, "The CBIN header does not frame the file.");
	std::vector<uint8_t> plain(data + kHeaderSize, data + size);
	apply_cipher(plain.data(), plain.size(), out.xor_key);
	const size_t table_end = string_offset - kHeaderSize;

	// The string table: `string_count` NUL-ended strings filling the blob.
	size_t at = table_end;
	for (uint32_t i = 0; i < string_count; ++i) {
		size_t end = at;
		while (end < plain.size() && plain[end] != 0) ++end;
		if (end >= plain.size()) return fail(error, "The CBIN string table is cut short.");
		out.strings.emplace_back(reinterpret_cast<const char *>(plain.data() + at), end - at);
		at = end + 1;
	}
	if (at != plain.size()) return fail(error, "The CBIN string table does not fill its blob.");

	size_t cursor = 0;
	const auto word = [&](uint32_t &value) {
		if (cursor + 4 > table_end) return false;
		value = io::read_u32_le(plain.data() + cursor);
		cursor += 4;
		return true;
	};
	// Each entry and each value takes two words: a count the table cannot hold is none.
	const size_t pairs = table_end / 8;
	uint32_t label_count = 0;
	if (!word(label_count)) return fail(error, "The CBIN label table is cut short.");
	std::vector<uint32_t> entry_counts;
	for (uint32_t i = 0; i < label_count; ++i) {
		BinaryConfig::Label label;
		uint32_t count = 0;
		if (!word(label.name) || !word(count)) return fail(error, "The CBIN label table is cut short.");
		// A count of 0xFFFFFFFF reads no block [orig: ConfigFile_ParseBinary @ 0x75ea73].
		label.count_minus_one = count == 0xFFFFFFFFu;
		if (label.count_minus_one) count = 0;
		if (count > pairs) return fail(error, "A CBIN label counts more entries than the file holds.");
		out.labels.push_back(std::move(label));
		entry_counts.push_back(count);
	}
	// Every label's name entries and its terminator after them, read as one more entry; a label of no entry
	// has neither, the reader stepping over its block alone where it counts entries [orig:
	// ConfigFile_ParseBinary @ 0x75ea65, the element block and its terminator read only for a count other
	// than 0, count + 1 pairs @ 0x75ea75, each name taken as it is @ 0x75ea7e; the writer @ 0x75e424..0x75e429,
	// its jz past them for a label of none].
	size_t values = 0;
	for (size_t i = 0; i < out.labels.size(); ++i) {
		if (entry_counts[i] == 0) continue;
		for (uint32_t j = 0; j <= entry_counts[i]; ++j) {
			uint32_t name = 0, type = 0;
			if (!word(name) || !word(type)) return fail(error, "The CBIN name table is cut short.");
			// Any value count, 0 and past two among them [orig: @ 0x75eb21].
			values += type;
			if (type > pairs || values > pairs) return fail(error, "A CBIN entry counts more values than the file holds.");
			BinaryConfig::Entry entry;
			entry.name = name;
			entry.values.resize(type);
			if (j == entry_counts[i])
				out.labels[i].terminator = std::move(entry);
			else
				out.labels[i].entries.push_back(std::move(entry));
		}
	}
	// Every entry's values, each label's terminator's after its entries', every word as it is [orig: the data
	// loop @ 0x75eb0b..0x75eb82, count + 1 entries; the flags @ 0x75eb40, the word @ 0x75eb47].
	for (BinaryConfig::Label &label : out.labels) {
		if (label.entries.empty()) continue;
		for (size_t j = 0; j <= label.entries.size(); ++j) {
			BinaryConfig::Entry &entry = j < label.entries.size() ? label.entries[j] : label.terminator;
			for (BinaryConfig::Value &value : entry.values)
				if (!word(value.raw) || !word(value.flags)) return fail(error, "The CBIN value table is cut short.");
		}
	}
	if (cursor != table_end) return fail(error, "The CBIN value table holds values no entry reads.");
	return true;
}

bool encode_binary_config(const BinaryConfig &config, std::vector<uint8_t> &out, std::string &error) {
	out.clear();
	std::vector<uint8_t> plain;
	io::append_u32_le(plain, static_cast<uint32_t>(config.labels.size()));
	for (const BinaryConfig::Label &label : config.labels) {
		// A label of no entry has no terminator pair to hold another.
		if (label.entries.empty() && (label.terminator.name != 0 || !label.terminator.values.empty()))
			return fail(error, "A label of no entry holds a terminator.");
		io::append_u32_le(plain, label.name);
		io::append_u32_le(plain, label.count_minus_one && label.entries.empty()
				? 0xFFFFFFFFu
				: static_cast<uint32_t>(label.entries.size()));
	}
	for (const BinaryConfig::Label &label : config.labels) {
		for (const BinaryConfig::Entry &entry : label.entries) {
			io::append_u32_le(plain, entry.name);
			io::append_u32_le(plain, static_cast<uint32_t>(entry.values.size()));
		}
		// The terminator closes a label's entries; a label of none writes neither [orig: the writer @ 0x75e429].
		if (label.entries.empty()) continue;
		io::append_u32_le(plain, label.terminator.name);
		io::append_u32_le(plain, static_cast<uint32_t>(label.terminator.values.size()));
	}
	for (const BinaryConfig::Label &label : config.labels) {
		if (label.entries.empty()) continue;
		for (size_t j = 0; j <= label.entries.size(); ++j) {
			const BinaryConfig::Entry &entry = j < label.entries.size() ? label.entries[j] : label.terminator;
			for (const BinaryConfig::Value &value : entry.values) {
				io::append_u32_le(plain, value.raw);
				io::append_u32_le(plain, value.flags);
			}
		}
	}
	const size_t string_offset = kHeaderSize + plain.size();
	for (const std::string &text : config.strings) {
		plain.insert(plain.end(), text.begin(), text.end());
		plain.push_back(0);
	}
	apply_cipher(plain.data(), plain.size(), config.xor_key);
	io::append_u32_le(out, kMagic);
	io::append_u32_le(out, static_cast<uint32_t>(string_offset));
	io::append_u32_le(out, static_cast<uint32_t>(kHeaderSize + plain.size() - string_offset));
	io::append_u32_le(out, static_cast<uint32_t>(config.strings.size()));
	io::append_u32_le(out, config.xor_key);
	out.insert(out.end(), plain.begin(), plain.end());
	return true;
}

// [orig: ConfigFile_ParseBinary @ 0x75e8a0]
bool binary_config_sections(const BinaryConfig &config, std::vector<configfile::ConfigSection> &out) {
	out.clear();
	// The string table the parse points into, each label's name lowercased in it [orig: strlwr @ 0x75e9d4]: a
	// label past the table is a stray pointer the parse writes through, no load. A null label is no pointer
	// the parse lowercases [orig: @ 0x75e9c4]; its section stays, null_label, a fault only for the lookup
	// whose scan reaches it (configfile::find_config_section). The table is the block's, one copy every
	// section shares [orig: the string table @ 0x75e949].
	auto block = std::make_shared<configfile::ConfigBlock>();
	block->strings = config.strings;
	std::vector<std::string> &strings = block->strings;
	for (const BinaryConfig::Label &label : config.labels) {
		if (label.name == 0) continue;
		if (label.name > strings.size()) return false;
		for (char &c : strings[label.name - 1])
			if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
	}
	const auto string_at = [&strings](uint32_t index) -> const std::string * {
		return index >= 1 && index <= strings.size() ? &strings[index - 1] : nullptr;
	};
	// The element block: every label's entries and its terminator, label after label, in one block every
	// section shares; each label's section points at its own block's first entry, a label counted
	// 0xFFFFFFFF at the next block's, a label of no entry at none [orig: the one allocation @ 0x75ea2f;
	// @ 0x75ea4e..0x75eabd, the pointer @ 0x75ea6f; the label table cleared @ 0x75e9aa].
	constexpr size_t kNone = SIZE_MAX;
	std::vector<bool> null_name;
	std::vector<size_t> first(config.labels.size(), kNone);
	const auto lay = [&](const BinaryConfig::Entry &entry) {
		configfile::ConfigEntry read;
		null_name.push_back(entry.name == 0);
		// The name is the table's string, a pointer into it, never a copy [orig: @ 0x75ea8e]; a name past the
		// table is a stray pointer the walk's stricmp faults on.
		if (string_at(entry.name) != nullptr)
			read.key_string = entry.name;
		else if (entry.name != 0)
			read.key_faults = true;
		// The value pointer is set whatever the count [orig: @ 0x75eb1e], so the walk goes past an
		// entry of none.
		read.value_pointer_set = true;
		for (const BinaryConfig::Value &value : entry.values) {
			// The flags are the type the accessors test: 1 an integer, 2 a float's bits, any other read as
			// text through the word and as the number 0 [orig: @ 0x75eb40; Effect_GetParamValue_0 @ 0x75fb2e /
			// @ 0x75fb37 / @ 0x75fb39..0x75fb58]. A set holding 4 points a nonzero word at its string [orig:
			// @ 0x75eb42..0x75eb5a]; a word of 0, or a string past the table, is a text read through a null
			// or stray pointer [orig: @ 0x75eb4e; String_CopyN @ 0x75eca4]: the stand-in fault, should a text
			// read reach it. Any other flags' text is the word as an address (D-CBIN-5): read here as no text.
			configfile::ConfigValue v;
			v.type = static_cast<int>(value.flags);
			if (value.flags == BinaryConfig::kInteger) {
				v.integer = static_cast<int32_t>(value.raw);
			} else if (value.flags == BinaryConfig::kFloat) {
				std::memcpy(&v.real, &value.raw, sizeof v.real);
			} else if (value.raw == 0) {
				v.text_faults = true;
			} else if ((value.flags & BinaryConfig::kString) != 0) {
				if (string_at(value.raw) != nullptr)
					v.text_string = value.raw; // a pointer into the table [orig: @ 0x75eb5a]
				else
					v.text_faults = true;
			}
			read.values.push_back(std::move(v));
		}
		block->entries.push_back(std::move(read));
	};
	for (size_t i = 0; i < config.labels.size(); ++i) {
		const BinaryConfig::Label &label = config.labels[i];
		if (label.entries.empty()) {
			if (label.count_minus_one) first[i] = block->entries.size();
			continue;
		}
		first[i] = block->entries.size();
		for (const BinaryConfig::Entry &entry : label.entries) lay(entry);
		lay(label.terminator);
	}
	// The walk from an entry runs to the first null name at or after it [orig: Effect_GetParamValue_0
	// @ 0x75fa00, the tests @ 0x75fa87 / @ 0x75faaf]; past the last block it would read the allocation's
	// slack (FastMem's rounding), bytes the parse never wrote: the entries end there, a read that walks on
	// failing.
	const size_t laid = block->entries.size();
	std::vector<size_t> stop(laid + 1, laid);
	for (size_t at = laid; at-- > 0;) stop[at] = null_name[at] ? at : stop[at + 1];
	for (size_t i = 0; i < config.labels.size(); ++i) {
		configfile::ConfigSection section;
		if (config.labels[i].name == 0)
			section.null_label = true;
		else
			section.label_string = config.labels[i].name; // a pointer into the table [orig: @ 0x75e9d2]
		section.block = block;
		if (first[i] != kNone) {
			section.first = first[i];
			section.count = stop[first[i]] - first[i];
		}
		out.push_back(std::move(section));
	}
	return true;
}

} // namespace opennova::cbin
