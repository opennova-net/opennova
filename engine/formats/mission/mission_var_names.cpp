#include <formats/mission/mission_var_names.h>

#include <algorithm>
#include <cstring>

namespace opennova::mission {

namespace {

constexpr size_t kRecord = bms::kLayerRecordSize;

bool record_is_zero(const bms::LayerRecord &record) {
	for (char ch : record.name)
		if (ch != 0) return false;
	return true;
}

bool record_is_tag(const bms::LayerRecord &record) {
	return std::memcmp(record.name, kVariableNamesTag, kRecord) == 0;
}

// The record's text: its bytes up to the first NUL (all 20 when none).
std::string record_text(const bms::LayerRecord &record) {
	size_t length = 0;
	while (length < kRecord && record.name[length] != 0) ++length;
	return std::string(record.name, length);
}

std::string prefix_of(int32_t index) { return "V" + std::to_string(index) + " "; }

// `V<index> <name>`: the digits without a leading zero (V0 alone may be 0), one space, a name.
bool parse_entry(const std::string &text, VariableName &out) {
	if (text.size() < 4 || text[0] != 'V') return false;
	size_t at = 1;
	int64_t index = 0;
	while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
		index = index * 10 + (text[at] - '0');
		if (index > kVariableNameIndexMax) return false;
		++at;
	}
	const size_t digits = at - 1;
	if (digits == 0 || (digits > 1 && text[1] == '0')) return false;
	if (at >= text.size() || text[at] != ' ' || at + 1 >= text.size()) return false;
	out.index = static_cast<int32_t>(index);
	out.name = text.substr(at + 1);
	return true;
}

bool name_refused(const VariableName &entry, std::string &error) {
	if (entry.index < 0 || entry.index > kVariableNameIndexMax) {
		error = "A variable name names V0..V" + std::to_string(kVariableNameIndexMax);
		return true;
	}
	const std::string &name = entry.name;
	if (name.empty()) {
		error = "A variable's name is empty";
		return true;
	}
	if (name.size() > variable_name_room(entry.index)) {
		error = "V" + std::to_string(entry.index) + "'s name holds " + std::to_string(variable_name_room(entry.index)) +
		        " characters at most";
		return true;
	}
	if (name.front() == ' ' || name.back() == ' ') {
		error = "A variable's name neither starts nor ends with a space";
		return true;
	}
	for (char ch : name) {
		const unsigned char c = static_cast<unsigned char>(ch);
		if (c > 0x7E || !bms::mis_value_char(ch)) {
			error = "A variable's name takes printable characters other than a double quote or a ';'";
			return true;
		}
	}
	return false;
}

} // namespace

LayerTable layer_table(const bms::File &file) {
	bool empty = true;
	for (const bms::LayerRecord &record : file.layer_records) empty = empty && record_is_zero(record);
	if (empty) return LayerTable::Empty;
	if (!file.layer_records.empty() && record_is_tag(file.layer_records[0])) return LayerTable::VariableNames;
	return LayerTable::LayerNames;
}

std::vector<VariableName> variable_names(const bms::File &file) {
	std::vector<VariableName> out;
	if (layer_table(file) != LayerTable::VariableNames) return out;
	for (size_t i = 1; i < file.layer_records.size(); ++i) {
		VariableName entry;
		std::string why;
		if (!parse_entry(record_text(file.layer_records[i]), entry) || name_refused(entry, why)) continue;
		const bool named = std::any_of(out.begin(), out.end(), [&](const VariableName &n) { return n.index == entry.index; });
		if (!named) out.push_back(std::move(entry));
	}
	return out;
}

size_t variable_name_room(int32_t index) {
	const size_t prefix = prefix_of(index).size();
	return prefix < kRecord ? kRecord - prefix : 0;
}

// The game reads the table and drops it, so the names change nothing it does [orig: Mission_LoadBMSFile
// @0x40f4e0, the layer loop @0x40fc05..0x40fc38]; the original editor's layer names are its own and stay.
bool set_variable_names(bms::File &file, const std::vector<VariableName> &names, std::string &error) {
	if (layer_table(file) == LayerTable::LayerNames) {
		error = "The mission's layer table holds the original editor's layer names; variable names are not "
		        "written over them";
		return false;
	}
	if (names.size() > kMaxVariableNames) {
		error = "A mission names " + std::to_string(kMaxVariableNames) + " variables at most";
		return false;
	}
	std::vector<VariableName> sorted = names;
	std::sort(sorted.begin(), sorted.end(),
	          [](const VariableName &a, const VariableName &b) { return a.index < b.index; });
	for (size_t i = 0; i < sorted.size(); ++i) {
		if (name_refused(sorted[i], error)) return false;
		if (i > 0 && sorted[i].index == sorted[i - 1].index) {
			error = "V" + std::to_string(sorted[i].index) + " is named twice";
			return false;
		}
	}
	file.layer_records.resize(bms::kLayerRecordCount);
	for (bms::LayerRecord &record : file.layer_records) std::memset(record.name, 0, kRecord);
	if (sorted.empty()) return true;
	std::memcpy(file.layer_records[0].name, kVariableNamesTag, kRecord);
	for (size_t i = 0; i < sorted.size(); ++i) {
		const std::string text = prefix_of(sorted[i].index) + sorted[i].name;
		std::memcpy(file.layer_records[i + 1].name, text.data(), text.size());
	}
	return true;
}

} // namespace opennova::mission
