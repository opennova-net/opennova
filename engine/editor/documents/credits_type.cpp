#include "credits_type.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>

#include <editor/model/text_document.h>
#include <formats/cbin/binary_config.h>
#include <formats/cbin/cbin.h>
#include <runtime/menu/config_text.h>

namespace opennova::editor {

namespace {

using cbin::BinaryConfig;

constexpr FindingCodeEntry<CreditsFinding> kFindingEntries[] = {
	{ CreditsFinding::InvalidInput, { "credits.invalid_input", FindingFix::None, nullptr, true } },
	{ CreditsFinding::Unserializable, { "credits.unserializable", FindingFix::None, nullptr, true } },
};
static_assert(std::size(kFindingEntries) == static_cast<size_t>(CreditsFinding::kCount),
		"every CreditsFinding has exactly one row");
static_assert(finding_entries_well_formed(kFindingEntries),
		"the credits' rows follow CreditsFinding's order, each token its own");
constexpr auto kFindingRows = finding_rows(kFindingEntries, FindingGroup::Credits);
static_assert(finding_rows_well_formed(kFindingRows), "every row of the table takes its group");

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
// the value a float: runtime/menu/config_text.cpp), with a point so it reads as a float; false for
// one no decimals reach (not a number, an infinity).
bool float_text(uint32_t bits, std::string &out) {
	const float value = bits_float(bits);
	if (!std::isfinite(value)) return false;
	char buffer[128];
	for (int decimals = 1; decimals <= 60; ++decimals) {
		std::snprintf(buffer, sizeof buffer, "%.*f", decimals, double(value));
		if (float_bits(float(std::atof(buffer))) == bits && menu::classify_numeric(buffer) == 2) {
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

// The text form of a CBIN file, or why it has none: what the text reader would read back as the
// file holds it. `why` says what does not go (the first such thing).
bool render(const BinaryConfig &config, std::string &text, std::string &why) {
	text.clear();
	for (const BinaryConfig::Label &label : config.labels) {
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
					written = *config.string_at(value.raw);
					if (!one_token(written) || menu::classify_numeric(written) != 0) {
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

// The 1-based index of a string in the table: its first place, else a place at the end.
uint32_t string_index(std::vector<std::string> &strings, const std::string &text) {
	for (size_t i = 0; i < strings.size(); ++i)
		if (strings[i] == text) return uint32_t(i + 1);
	strings.push_back(text);
	return uint32_t(strings.size());
}

// The text read as the game's text reader reads it, in the CBIN form under the file's key, its
// string table first in its order; false with the issue for a text that does not go in it.
class CreditsEncoding : public TextEncoding {
public:
	CreditsEncoding(std::vector<std::string> strings, uint32_t key) :
			strings_(std::move(strings)), key_(key) {}

	bool encode(const std::string &text, std::string &stored,
			std::vector<SourceIssue> &issues) const override {
		BinaryConfig config;
		config.strings = strings_;
		config.xor_key = key_;
		const std::vector<menu::ConfigSection> sections =
				menu::parse_config_text(reinterpret_cast<const uint8_t *>(text.data()), text.size());
		for (const menu::ConfigSection &section : sections) {
			BinaryConfig::Label label;
			label.name = string_index(config.strings, section.label);
			for (const menu::ConfigEntry &read : section.entries) {
				if (read.values.size() > 2) {
					issues.push_back({true, 0, std::string(), std::string(),
							"The CBIN form holds one or two values an entry: \"" + read.key +
									"\" in [" + section.label + "] holds " +
									std::to_string(read.values.size()) + "."});
					return false;
				}
				BinaryConfig::Entry entry;
				entry.name = string_index(config.strings, read.key);
				for (const menu::ConfigValue &value : read.values) {
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
		std::vector<uint8_t> bytes;
		std::string error;
		if (!cbin::encode_binary_config(config, bytes, error)) {
			issues.push_back({true, 0, std::string(), std::string(), error});
			return false;
		}
		stored.assign(bytes.begin(), bytes.end());
		return true;
	}

private:
	std::vector<std::string> strings_;
	uint32_t key_;
};

bool decode_credits(const std::vector<uint8_t> &stored, std::string &text,
		std::shared_ptr<const TextEncoding> &encoding, std::vector<SourceIssue> &issues,
		std::string &error) {
	if (!cbin::is_cbin(stored.data(), stored.size())) {
		text.assign(stored.begin(), stored.end()); // the text form: the file is its text
		return true;
	}
	BinaryConfig config;
	if (!cbin::decode_binary_config(stored.data(), stored.size(), config, error)) return false;
	auto kept = std::make_shared<CreditsEncoding>(config.strings, config.xor_key);
	std::string why;
	if (!render(config, text, why)) {
		issues.push_back({true, 0, std::string(), std::string(),
				"This credits file's CBIN form holds what its text form cannot carry (" + why +
						"): the editor shows the rest and does not save it."});
		encoding = std::move(kept);
		return true;
	}
	// The text written back is the file it was read from, or the file is held read only.
	std::string again;
	std::vector<SourceIssue> refused;
	if (!kept->encode(text, again, refused) ||
			again.size() != stored.size() ||
			std::memcmp(again.data(), stored.data(), stored.size()) != 0)
		issues.push_back({true, 0, std::string(), std::string(),
				"This credits file's CBIN form would not be written back as it is from its text "
				"(its string table holds a text twice, or what reads back differs): the editor "
				"shows it and does not save it."});
	encoding = std::move(kept);
	return true;
}

} // namespace

std::unique_ptr<DocumentBase> make_credits_document() {
	return std::make_unique<TextDocument>(decode_credits);
}

std::vector<Diagnostic> validate_credits_file(const DocumentBase &document) {
	std::vector<Diagnostic> findings;
	if (!text_of(document)) return findings;
	for (const SourceIssue &issue : document.issues())
		findings.push_back(make_finding(CreditsFinding::InvalidInput, DiagnosticSeverity::Warning,
				issue.message, document.path()));
	if (document.blocked()) return findings;
	const SerializeResult written = document.serialize();
	for (const SourceIssue &issue : written.issues)
		findings.push_back(make_finding(CreditsFinding::Unserializable, DiagnosticSeverity::Error,
				issue.message, document.path()));
	return findings;
}

const FindingCodeRow &finding_code(CreditsFinding code) {
	return kFindingRows[static_cast<size_t>(code)];
}

FindingTable credits_finding_codes() {
	return { kFindingRows.data(), kFindingRows.size() };
}

} // namespace opennova::editor
