#include <formats/cbin/binary_config_text.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <base/io/strutil.h>
#include <formats/configfile/config_file.h>

namespace opennova::cbin {

namespace {

uint32_t float_bits(float value) {
	uint32_t bits = 0;
	std::memcpy(&bits, &value, sizeof bits);
	return bits;
}

float bits_float(uint32_t bits) {
	float value = 0.0f;
	std::memcpy(&value, &bits, sizeof value);
	return value;
}

// A float written in the fewest decimals the text reader reads back to its very bits (its atof,
// the value a float: formats/configfile/config_file.cpp), with a point so it reads as a float; false for
// one no decimals reach (not a number, an infinity).
bool float_text(uint32_t bits, std::string &out) {
	const float value = bits_float(bits);
	if (!std::isfinite(value)) return false;
	char buffer[128];
	for (int decimals = 1; decimals <= 60; ++decimals) {
		std::snprintf(buffer, sizeof buffer, "%.*f", decimals, double(value));
		if (float_bits(float(std::atof(buffer))) == bits && configfile::classify_numeric(buffer) == 2) {
			out = buffer;
			return true;
		}
	}
	return false;
}

// A text the reader takes as one token: something, with no separator (a blank, ',', ';', a line's
// end, a NUL) in it.
bool one_token(const std::string &text) {
	if (text.empty()) return false;
	for (const char c : text)
		if (c == ' ' || c == '\t' || c == ',' || c == ';' || c == '\r' || c == '\n' || c == '\0')
			return false;
	return true;
}

bool blank(const std::string &text) {
	return text.find_first_not_of(" \t") == std::string::npos;
}

// What the reader reads of an entry line (in a section): its name up to the first ';', CR, LF or
// '=', which must be the '='; its value up to the first ';', CR or LF; its values the runs between
// ',' and ' ' [orig: ConfigFile_ParseText @ 0x7608a0; formats/configfile/config_file.cpp]. "" when it
// reads the line whole, else why not.
std::string entry_unread(const std::string &line) {
	const size_t key_end = line.find_first_of(";\r\n=");
	if (key_end == std::string::npos)
		return "holds no '=', so the reader reads no entry of it";
	if (line[key_end] == ';')
		return key_end == line.find_first_not_of(" \t") ? "is a comment (';'), which the reader reads none of"
		                                                : "holds a ';' before its '=', so the reader reads no entry of it";
	if (line[key_end] != '=')
		return std::string("holds ") + (line[key_end] == '\r' ? "a CR" : "an LF") +
		       " alone before its '=', so the reader reads no entry of it (it ends a line at CR LF)";
	if (blank(line.substr(0, key_end))) return "names no entry before its '='";
	const size_t value_end = std::min(line.size(), line.find_first_of(";\r\n", key_end + 1));
	const std::string value = line.substr(key_end + 1, value_end - key_end - 1);
	if (value.find_first_not_of(" ,\t") == std::string::npos) return "holds no value after its '='";
	if (value_end < line.size()) {
		if (line[value_end] == ';')
			return "goes on after its values with a ';' comment, which the reader does not read";
		return std::string("holds ") + (line[value_end] == '\r' ? "a CR" : "an LF") +
		       " alone, where the reader stops reading the line (it ends a line at CR LF)";
	}
	return std::string();
}

// The 1-based index of a string in the table: its first place, else a place at the end.
uint32_t string_index(std::vector<std::string> &strings, const std::string &text) {
	for (size_t i = 0; i < strings.size(); ++i)
		if (strings[i] == text) return uint32_t(i + 1);
	strings.push_back(text);
	return uint32_t(strings.size());
}

} // namespace

bool binary_config_text(const BinaryConfig &config, std::string &text, std::string &why) {
	text.clear();
	for (const BinaryConfig::Label &label : config.labels) {
		if (!config.string_at(label.name)) {
			why = "a label names a string past its string table";
			return false;
		}
		const std::string &name = *config.string_at(label.name);
		std::string upper;
		for (const char c : name) {
			if (c >= 'a' && c <= 'z') {
				upper.push_back(char(c - 'a' + 'A'));
			} else if ((c >= '0' && c <= '9') || c == '_') {
				upper.push_back(c);
			} else {
				why = "its label \"" + name + "\" holds a character a section line cannot carry";
				return false;
			}
		}
		if (upper.empty()) {
			why = "it has a label of no name";
			return false;
		}
		text += "[" + upper + "]\r\n";
		for (const BinaryConfig::Entry &entry : label.entries) {
			if (!config.string_at(entry.name)) {
				why = "an entry names a string past its string table";
				return false;
			}
			const std::string &key = *config.string_at(entry.name);
			if (key.empty() || key.front() == '[' || key.front() == ' ' || key.back() == ' ' ||
					key.find_first_of("=;\t\r\n") != std::string::npos || key.find('\0') != std::string::npos) {
				why = "its entry name \"" + key + "\" is not one a line's key can carry";
				return false;
			}
			text += key + " =";
			for (size_t i = 0; i < entry.values.size(); ++i) {
				const BinaryConfig::Value &value = entry.values[i];
				std::string written;
				if (value.flags == BinaryConfig::kInteger) {
					written = std::to_string(int32_t(value.raw));
				} else if (value.flags == BinaryConfig::kFloat) {
					if (!float_text(value.raw, written)) {
						why = "an entry \"" + key + "\" holds a float that is no number";
						return false;
					}
				} else if (value.flags == BinaryConfig::kString) {
					if (!config.string_at(value.raw)) {
						why = "the entry \"" + key + "\" holds a string past its string table";
						return false;
					}
					written = *config.string_at(value.raw);
					if (!one_token(written) || configfile::classify_numeric(written) != 0) {
						why = "the entry \"" + key + "\" holds the text \"" + written +
								"\", which its text form would read otherwise";
						return false;
					}
				} else {
					why = "the entry \"" + key + "\" holds a value of flags " +
							std::to_string(value.flags) + ", none of an integer, a float or a string";
					return false;
				}
				text += (i == 0 ? " " : ", ") + written;
			}
			text += "\r\n";
		}
	}
	return true;
}

bool binary_config_text_readable(const std::string &text, ConfigTextRefusal &refusal) {
	const auto refuse = [&refusal](size_t line, const std::string &why) {
		refusal.line = line;
		refusal.why = why;
		return false;
	};
	bool in_section = false;
	size_t number = 0;
	// The lines as the reader splits them, at CR LF.
	for (size_t start = 0; start <= text.size();) {
		const size_t found = text.find("\r\n", start);
		const size_t end = found == std::string::npos ? text.size() : found;
		const std::string line = text.substr(start, end - start);
		++number;
		start = found == std::string::npos ? text.size() + 1 : found + 2;
		const size_t nul = line.find('\0');
		if (nul != std::string::npos && !blank(line.substr(nul + 1)))
			return refuse(number, "holds a NUL, after which the reader reads nothing of it");
		const std::string read = line.substr(0, nul);
		if (blank(read)) continue;
		if (read[0] == '[') {
			size_t label = 1;
			while (label < read.size() && ((read[label] >= 'A' && read[label] <= 'Z') ||
			                               (read[label] >= '0' && read[label] <= '9') || read[label] == '_'))
				++label;
			if (label == 1)
				return refuse(number, "opens no section: the reader takes a label in capitals, digits and '_', "
				                      "and reads none of the entries after it");
			const size_t rest = label < read.size() && read[label] == ']' ? label + 1 : label;
			if (!blank(read.substr(rest)))
				return refuse(number, "holds more than its section's label, which the reader does not read");
			in_section = true;
			continue;
		}
		if (strutil::trim_view(read).front() == '[')
			return refuse(number, "starts its section after a blank: the reader looks for '[' in a line's "
			                      "first column, and reads none of it");
		if (!in_section) return refuse(number, "is outside any section, where the reader reads nothing");
		const std::string unread = entry_unread(read);
		if (!unread.empty()) return refuse(number, unread);
		// Its values, as the reader splits them: one or two go in the form.
		const std::vector<configfile::ConfigSection> parsed =
				configfile::parse_config_text(reinterpret_cast<const uint8_t *>(("[X]\r\n" + read).data()), read.size() + 5);
		const size_t values = parsed.empty() || parsed[0].entries.empty() ? 0 : parsed[0].entries[0].values.size();
		if (values > 2)
			return refuse(number, "holds " + std::to_string(values) +
			                              " values, where an entry of the CBIN form holds one or two");
	}
	return true;
}

BinaryConfig binary_config_from_text(const std::string &text, const std::vector<std::string> &strings,
		uint32_t key) {
	BinaryConfig config;
	config.strings = strings;
	config.xor_key = key;
	const std::vector<configfile::ConfigSection> sections =
			configfile::parse_config_text(reinterpret_cast<const uint8_t *>(text.data()), text.size());
	for (const configfile::ConfigSection &section : sections) {
		BinaryConfig::Label label;
		label.name = string_index(config.strings, section.label);
		for (const configfile::ConfigEntry &read : section.entries) {
			BinaryConfig::Entry entry;
			entry.name = string_index(config.strings, read.key);
			for (const configfile::ConfigValue &value : read.values) {
				BinaryConfig::Value written;
				if (value.type == 1) {
					written = { uint32_t(value.integer), BinaryConfig::kInteger };
				} else if (value.type == 2) {
					written = { float_bits(value.real), BinaryConfig::kFloat };
				} else {
					written = { string_index(config.strings, value.text), BinaryConfig::kString };
				}
				entry.values.push_back(written);
			}
			label.entries.push_back(std::move(entry));
		}
		config.labels.push_back(std::move(label));
	}
	return config;
}

} // namespace opennova::cbin
