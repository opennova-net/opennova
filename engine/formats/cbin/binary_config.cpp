// The CBIN form read and written exactly (binary_config.h). The layout is the one cbin.cpp's
// Credits reader walks and its writer lays out [orig: the writer @ 0x75e250: magic @ 0x75e311,
// the 20-byte header, the cipher loop @ 0x75e348], read here whatever the labels.
#include <formats/cbin/binary_config.h>

#include <base/io/le.h>
#include <formats/cbin/cbin.h>
#include <formats/configfile/config_file.h>

#include <cstring>

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
	uint32_t label_count = 0;
	if (!word(label_count)) return fail(error, "The CBIN label table is cut short.");
	std::vector<uint32_t> entry_counts;
	for (uint32_t i = 0; i < label_count; ++i) {
		BinaryConfig::Label label;
		uint32_t count = 0;
		if (!word(label.name) || !word(count)) return fail(error, "The CBIN label table is cut short.");
		if (!out.string_at(label.name)) return fail(error, "A CBIN label names no string.");
		// Each entry takes two words at least: a count the table cannot hold is none.
		if (count > table_end / 8) return fail(error, "A CBIN label counts more entries than the file holds.");
		out.labels.push_back(std::move(label));
		entry_counts.push_back(count);
	}
	// Every label's name entries, each list closed by its terminator.
	for (size_t i = 0; i < out.labels.size(); ++i) {
		for (uint32_t j = 0; j <= entry_counts[i]; ++j) {
			uint32_t name = 0, type = 0;
			if (!word(name) || !word(type)) return fail(error, "The CBIN name table is cut short.");
			if (j == entry_counts[i]) {
				if (name != 0 || type != 0) return fail(error, "A CBIN label's entries are not closed by its terminator.");
				continue;
			}
			if (type < 1 || type > 2) return fail(error, "A CBIN entry holds no value or more than two.");
			if (!out.string_at(name)) return fail(error, "A CBIN entry names no string.");
			BinaryConfig::Entry entry;
			entry.name = name;
			entry.values.resize(type);
			out.labels[i].entries.push_back(std::move(entry));
		}
	}
	// Every entry's values, in the same order, filling the table.
	for (BinaryConfig::Label &label : out.labels)
		for (BinaryConfig::Entry &entry : label.entries)
			for (BinaryConfig::Value &value : entry.values) {
				if (!word(value.raw) || !word(value.flags)) return fail(error, "The CBIN value table is cut short.");
				if (value.flags == BinaryConfig::kString && !out.string_at(value.raw))
					return fail(error, "A CBIN value names no string.");
			}
	if (cursor != table_end) return fail(error, "The CBIN value table holds values no entry reads.");
	return true;
}

bool encode_binary_config(const BinaryConfig &config, std::vector<uint8_t> &out, std::string &error) {
	out.clear();
	std::vector<uint8_t> plain;
	io::append_u32_le(plain, static_cast<uint32_t>(config.labels.size()));
	for (const BinaryConfig::Label &label : config.labels) {
		if (!config.string_at(label.name)) return fail(error, "A label names no string.");
		io::append_u32_le(plain, label.name);
		io::append_u32_le(plain, static_cast<uint32_t>(label.entries.size()));
	}
	for (const BinaryConfig::Label &label : config.labels) {
		for (const BinaryConfig::Entry &entry : label.entries) {
			if (entry.values.empty() || entry.values.size() > 2)
				return fail(error, "An entry holds no value or more than two.");
			if (!config.string_at(entry.name)) return fail(error, "An entry names no string.");
			io::append_u32_le(plain, entry.name);
			io::append_u32_le(plain, static_cast<uint32_t>(entry.values.size()));
		}
		io::append_u32_le(plain, 0);
		io::append_u32_le(plain, 0);
	}
	for (const BinaryConfig::Label &label : config.labels)
		for (const BinaryConfig::Entry &entry : label.entries)
			for (const BinaryConfig::Value &value : entry.values) {
				if (value.flags == BinaryConfig::kString && !config.string_at(value.raw))
					return fail(error, "A value names no string.");
				io::append_u32_le(plain, value.raw);
				io::append_u32_le(plain, value.flags);
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
	// The string table the parse points into, each label's name lowercased in it [orig: strlwr @ 0x75e9d4].
	std::vector<std::string> strings = config.strings;
	for (const BinaryConfig::Label &label : config.labels) {
		if (label.name < 1 || label.name > strings.size()) return false;
		for (char &c : strings[label.name - 1])
			if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
	}
	const auto string_at = [&strings](uint32_t index) -> const std::string * {
		return index >= 1 && index <= strings.size() ? &strings[index - 1] : nullptr;
	};
	for (const BinaryConfig::Label &label : config.labels) {
		configfile::ConfigSection section;
		section.label = *string_at(label.name);
		for (const BinaryConfig::Entry &entry : label.entries) {
			const std::string *name = string_at(entry.name);
			if (name == nullptr) {
				out.clear();
				return false;
			}
			configfile::ConfigEntry read;
			read.key = *name;
			for (const BinaryConfig::Value &value : entry.values) {
				configfile::ConfigValue v;
				if (value.flags == BinaryConfig::kInteger) {
					v.type = 1;
					v.integer = static_cast<int32_t>(value.raw);
				} else if (value.flags == BinaryConfig::kFloat) {
					v.type = 2;
					std::memcpy(&v.real, &value.raw, sizeof v.real);
				} else if ((value.flags & BinaryConfig::kString) != 0 && string_at(value.raw) != nullptr) {
					v.type = 4;
					v.text = *string_at(value.raw);
				} else {
					out.clear();
					return false;
				}
				read.values.push_back(std::move(v));
			}
			section.entries.push_back(std::move(read));
		}
		out.push_back(std::move(section));
	}
	return true;
}

} // namespace opennova::cbin
