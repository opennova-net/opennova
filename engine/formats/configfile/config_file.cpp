// The ConfigFile's text form and its accessors (config_file.h) [orig: ConfigFile_LoadFromFile @ 0x760a10 ->
// ConfigFile_ParseText @ 0x7608a0; ConfigFile_FindSection @ 0x75eeb0; effect_get_param_value_0 @ 0x75fa00;
// effect_get_param_value @ 0x75f580].

#include <formats/configfile/config_file.h>

#include <base/io/crt_ftol.h>
#include <base/io/strutil.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace opennova::configfile {

namespace {

// strtol over retail's 32-bit long, saturating at its range whatever this platform's long is (a
// 64-bit long would let a value past it wrap in the cast).
int32_t strtol32(const char *text) {
	const long long v = std::strtoll(text, nullptr, 10);
	if (v > INT32_MAX) return INT32_MAX;
	if (v < INT32_MIN) return INT32_MIN;
	return static_cast<int32_t>(v);
}

// sscanf "%[^<stop>]": one or more characters not in `stop`.
size_t scan_run(const std::string &line, size_t at, const char *stop) {
	size_t end = at;
	while (end < line.size() && std::strchr(stop, line[end]) == nullptr) ++end;
	return end;
}

// A value as the accessors print it [orig: sprintf "%d" @ 0x75fc11 / "%f" @ 0x75fbae].
std::string printed(const ConfigValue &v) {
	if (v.type == 1) return std::to_string(v.integer);
	if (v.type == 2) {
		char buffer[512];
		std::snprintf(buffer, sizeof buffer, "%f", static_cast<double>(v.real));
		return buffer;
	}
	return v.text;
}

// One value into the accessor's outputs [orig: @ 0x75fb2e..0x75fc36]: an integer read as a float
// converts (fild), a float read as an integer truncates (_ftol2_sse), a string reads as 0.
void store(const ConfigValue &v, std::string *text, float *real, int32_t *integer) {
	if (v.type == 1) {
		if (real != nullptr) *real = static_cast<float>(v.integer);
		if (integer != nullptr) *integer = v.integer;
	} else if (v.type == 2) {
		if (real != nullptr) *real = v.real;
		if (integer != nullptr) *integer = io::retail_ftol_sse2(static_cast<double>(v.real));
	}
	if (text != nullptr) *text = printed(v);
}

} // namespace

// [orig: String_ClassifyNumeric @ 0x75d830]: 0 string, 1 integer, 2 float.
int classify_numeric(const std::string &s) {
	bool has_dot = false;
	bool has_sign = false;
	size_t p = 0;
	if (s.empty()) return 0;
	while (!std::isdigit(static_cast<unsigned char>(s[p]))) {
		switch (s[p]) {
			case ' ':
				if (has_dot) return 0;
				break;
			case '+':
			case '-':
				if (has_sign) return 0;
				has_sign = true;
				break;
			case '.':
				if (has_dot) return 0;
				has_dot = true;
				break;
			default:
				return 0;
		}
		if (++p >= s.size()) return 0;
	}
	while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) {
		++p;
		if (p < s.size() && s[p] == '.') {
			if (has_dot) return 0;
			has_dot = true;
			++p;
		} else if (p < s.size() && (s[p] == '-' || s[p] == '+')) {
			return 0;
		}
	}
	return p >= s.size() ? (has_dot ? 2 : 1) : 0;
}

std::vector<ConfigSection> parse_config_text(const uint8_t *data, size_t size) {
	// The lines, each with its first byte's offset into the text.
	std::vector<std::pair<std::string, size_t>> lines(1, {std::string(), 0});
	for (size_t i = 0; i < size; ++i) {
		const char c = static_cast<char>(data[i]);
		if (c == '\r' && i + 1 < size && data[i + 1] == '\n') {
			lines.back().first += '\n';
			lines.emplace_back(std::string(), i + 2);
			++i;
			continue;
		}
		lines.back().first += c == '\t' ? ' ' : c;
	}
	lines.back().first += '\n';
	std::vector<ConfigSection> sections;
	ConfigSection *section = nullptr;
	for (const auto &[line, line_offset] : lines) {
		const std::string text = line.substr(0, line.find('\0'));
		if (!text.empty() && text[0] == '[') {
			size_t end = 1;
			while (end < text.size() && (std::isupper(static_cast<unsigned char>(text[end])) ||
												std::isdigit(static_cast<unsigned char>(text[end])) ||
												text[end] == '_'))
				++end;
			if (end > 1) {
				sections.emplace_back();
				sections.back().label = strutil::to_lower(text.substr(1, end - 1));
				sections.back().offset = line_offset;
				section = &sections.back();
			} else {
				section = nullptr; // a '[' line ends the entries without opening a section
			}
			continue;
		}
		if (section == nullptr) continue;
		const size_t key_end = scan_run(text, 0, ";\n\r=");
		if (key_end == 0 || key_end >= text.size() || text[key_end] != '=') continue;
		const size_t value_end = scan_run(text, key_end + 1, "\n\r;");
		if (value_end == key_end + 1) continue;
		ConfigEntry entry;
		entry.key = std::string(strutil::trim_view(text.substr(0, key_end)));
		entry.offset = line_offset;
		const size_t value_start = key_end + 1;
		const std::string value = text.substr(value_start, value_end - value_start);
		size_t p = 0;
		while (p < value.size()) {
			while (p < value.size() && (value[p] == ',' || value[p] == ' ')) ++p;
			const size_t start = p;
			while (p < value.size() && value[p] != ',' && value[p] != ' ') ++p;
			if (p == start) break;
			ConfigValue v;
			v.text = value.substr(start, p - start);
			v.offset = line_offset + value_start + start;
			v.type = classify_numeric(v.text);
			if (v.type == 1) {
				v.integer = strtol32(v.text.c_str());
			} else if (v.type == 2) {
				v.real = static_cast<float>(io::retail_atof(v.text.c_str()));
			} else {
				v.type = 4;
			}
			entry.values.push_back(std::move(v));
		}
		section->entries.push_back(std::move(entry));
	}
	return sections;
}

ConfigSection *find_config_section(std::vector<ConfigSection> &sections, const char *name) {
	const std::string label = strutil::to_lower(name);
	for (ConfigSection &s : sections) {
		if (!strutil::iequals(s.label, label)) continue;
		s.current = 0;
		s.next = SIZE_MAX;
		return &s;
	}
	return nullptr;
}

bool read_config_value(ConfigSection &s, const char *key, int index, std::string *text, float *real,
		int32_t *integer) {
	if (real != nullptr) *real = 0.0f;
	if (integer != nullptr) *integer = 0;
	size_t at = s.next != SIZE_MAX ? s.next : s.current;
	if (index < 1 || at >= s.entries.size()) return false;
	while (!strutil::iequals(s.entries[at].key, key)) {
		s.current = at;
		if (s.entries[at].values.empty()) return false;
		s.next = ++at;
		if (at >= s.entries.size()) return false;
	}
	s.current = at;
	s.next = at + 1;
	const ConfigEntry &entry = s.entries[at];
	if (static_cast<size_t>(index) > entry.values.size()) return false;
	store(entry.values[static_cast<size_t>(index - 1)], text, real, integer);
	return true;
}

bool read_current_config_value(ConfigSection &s, const char *key, int index, std::string *text, float *real,
		int32_t *integer) {
	if (real != nullptr) *real = 0.0f;
	if (integer != nullptr) *integer = 0;
	if (index < 1 || s.current >= s.entries.size() || !strutil::iequals(s.entries[s.current].key, key))
		return false;
	const ConfigEntry &entry = s.entries[s.current];
	s.next = s.current + 1;
	if (static_cast<size_t>(index) > entry.values.size()) return false;
	store(entry.values[static_cast<size_t>(index - 1)], text, real, integer);
	return true;
}

// ---- the data-strings pool (config_file.h DataStringsPool) ----

namespace {

// The text as the parse holds it [orig: ConfigFile_LoadFromFile @ 0x760a3c..0x760a5a, the file's bytes and
// then an LF and a NUL; ConfigFile_ParseText @ 0x7608bb..0x7608db, each CR LF an LF and a NUL (the LF after
// the file's last byte counting), each tab a space, the lines +16 one more for each CR LF from the 1 the load
// sets @ 0x760a93].
struct ReaderText {
	std::string bytes;
	uint32_t lines = 1;
	std::vector<size_t> nuls; // where each NUL is: a line's index is the NULs before it
};

ReaderText reader_text(const uint8_t *data, size_t size) {
	ReaderText r;
	r.bytes.assign(reinterpret_cast<const char *>(data), size);
	r.bytes += '\n';
	r.bytes += '\0';
	for (size_t i = 0; i < size; ++i) {
		if (r.bytes[i] == '\r' && r.bytes[i + 1] == '\n') {
			r.bytes[i] = '\n';
			r.bytes[i + 1] = '\0';
			++r.lines;
		}
		if (r.bytes[i] == '\t') r.bytes[i] = ' ';
	}
	for (size_t i = 0; i < r.bytes.size(); ++i)
		if (r.bytes[i] == '\0') r.nuls.push_back(i);
	return r;
}

// String_CopyN(buffer, at, 256): to the first NUL, 255 bytes at most [orig: String_CopyN @ 0x75eca0].
std::string copy_line(const std::string &bytes, size_t at) {
	size_t end = at;
	while (end < bytes.size() && bytes[end] != '\0' && end - at < 255) ++end;
	return bytes.substr(at, end - at);
}

// sscanf(line, "%[^\n\r=]=%[^\n\r;]"): the conversions made (0, 1 or 2), the key and the value.
int scan_key_value(const std::string &line, std::string &key, std::string &value) {
	const size_t key_end = scan_run(line, 0, "\n\r=");
	if (key_end == 0) return 0;
	key = line.substr(0, key_end);
	if (key_end >= line.size() || line[key_end] != '=') return 1;
	const size_t value_end = scan_run(line, key_end + 1, "\n\r;");
	if (value_end == key_end + 1) return 1;
	value = line.substr(key_end + 1, value_end - key_end - 1);
	return 2;
}

// The key's trailing spaces cut, its first character kept [orig: @ 0x75fdd9..0x75fdf7].
void trim_key(std::string &key) {
	while (key.size() > 1 && key.back() == ' ') key.pop_back();
}

// The value's token `index` (1-based) as strtok over ", " finds it; false for none.
bool nth_token(const std::string &value, int index, std::string &out) {
	size_t p = 0;
	for (int n = 1;; ++n) {
		while (p < value.size() && (value[p] == ',' || value[p] == ' ')) ++p;
		if (p >= value.size()) return false;
		const size_t start = p;
		while (p < value.size() && value[p] != ',' && value[p] != ' ') ++p;
		if (n == index) {
			out = value.substr(start, p - start);
			return true;
		}
	}
}

// One read of the walk [orig: ConfigFile_ReadKeyValue @ 0x75fd30..0x75fe0c, its simple mode, which the parse
// runs in]: from the cursor (+8) and its line (+20) on, while the line is under the count (+16), each line
// copied and stepped past, the line read kept (+12); a '[' line (or an empty one) ends it unread; a line whose
// key, its trailing spaces cut, matches `key` (its first character exactly, then without case) is the one
// read, its value the last the scan converted (a line of a key and no value keeps the one before). Its value
// `index` is the token; false where the walk ends unread or the value has none.
struct Walk {
	size_t cursor = 0;
	uint32_t line = 0;
	size_t current = 0; // +12
};

bool walk_read(const ReaderText &r, Walk &w, const std::string &key, std::string &token) {
	std::string value;
	while (w.line < r.lines && w.cursor < r.bytes.size()) {
		const std::string line = copy_line(r.bytes, w.cursor);
		w.current = w.cursor;
		w.cursor += line.size() + 1;
		++w.line;
		if (line.empty() || line[0] == '[') return false;
		std::string read_key, read_value;
		const int converted = scan_key_value(line, read_key, read_value);
		if (converted == 0) continue;
		if (converted == 2) value = read_value;
		trim_key(read_key);
		if (!key.empty() && read_key[0] == key[0] && strutil::iequals(read_key, key)) return nth_token(value, 1, token);
	}
	return false;
}

// A further value [orig: ini_read_key_value_from_current_line @ 0x75f780, its simple mode]: the line the walk
// read last (+12), unread where it is a '[' line, holds no key and value, or its key (trailing spaces cut) is
// not `key` without case; else its token `index`.
bool current_read(const ReaderText &r, const Walk &w, const std::string &key, int index, std::string &token) {
	const std::string line = copy_line(r.bytes, w.current);
	if (line.empty() || line[0] == '[') return false;
	std::string read_key, read_value;
	if (scan_key_value(line, read_key, read_value) != 2) return false;
	trim_key(read_key);
	if (!strutil::iequals(read_key, key)) return false;
	return nth_token(read_value, index, token);
}

} // namespace

uint32_t fastmem_block_bytes(uint32_t size) {
	const uint32_t asked = size < 1 ? 1 : size;
	return (asked + (kFastMemStep - 1)) / kFastMemStep * kFastMemStep;
}

DataStringsPool data_strings_pool(const uint8_t *data, size_t size) {
	DataStringsPool pool;
	if (data == nullptr || size == 0) return pool;
	if (size >= 4 && std::memcmp(data, "CBIN", 4) == 0) {
		pool.binary = true;
		return pool;
	}
	const std::vector<ConfigSection> sections = parse_config_text(data, size);
	const ReaderText r = reader_text(data, size);
	std::string held; // the count's 256-byte buffer [orig: @ 0x7605d0, str]
	Walk w;
	// [orig: ConfigFile_CountValuesAndStringLengths @ 0x7605d0: every section, every entry, every value].
	for (const ConfigSection &section : sections) {
		for (const ConfigEntry &entry : section.entries) {
			const size_t count = entry.values.size();
			pool.values += static_cast<uint32_t>(count);
			// [orig: ConfigFile_CountCommaSeparatedValues @ 0x75de6a / 0x75de71: the line and its cursor].
			w.cursor = entry.offset;
			w.line = static_cast<uint32_t>(std::lower_bound(r.nuls.begin(), r.nuls.end(), entry.offset) - r.nuls.begin());
			for (size_t index = 1; index <= count; ++index) {
				std::string token;
				const bool read = index == 1 ? walk_read(r, w, entry.key, token)
				                             : current_read(r, w, entry.key, static_cast<int>(index), token);
				if (read) held = token.substr(0, 255);
				// [orig: @ 0x76067e..0x76069f: a value String_ClassifyNumeric calls text adds its length and one].
				if (classify_numeric(held) == 0) pool.string_bytes += static_cast<uint32_t>(held.size() + 1);
			}
		}
	}
	pool.pool_bytes = fastmem_block_bytes(pool.string_bytes);
	return pool;
}

size_t data_strings_overrun_offset(const std::vector<ConfigSection> &sections, const DataStringsPool &pool) {
	// The value whose byte of the clear is the first past the pool, in the reader's order.
	size_t offset = 0;
	size_t seen = 0;
	for (const ConfigSection &section : sections)
		for (const ConfigEntry &entry : section.entries)
			for (const ConfigValue &value : entry.values)
				if (seen++ == pool.pool_bytes) offset = value.offset;
	return offset;
}

std::string config_commented(const std::string &text, std::vector<size_t> line_starts) {
	std::sort(line_starts.begin(), line_starts.end());
	line_starts.erase(std::unique(line_starts.begin(), line_starts.end()), line_starts.end());
	std::string out;
	out.reserve(text.size() + line_starts.size());
	size_t from = 0;
	for (const size_t at : line_starts) {
		if (at > text.size()) break;
		out.append(text, from, at - from);
		out += ';';
		from = at;
	}
	out.append(text, from, std::string::npos);
	return out;
}

} // namespace opennova::configfile
