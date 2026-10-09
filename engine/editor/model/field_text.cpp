#include "field_text.h"

#include <base/io/strutil.h>

#include <cstdio>

namespace opennova::editor {

std::string field_title(const FieldSchema &field) { return field.label.empty() ? field.id : field.label; }

std::string field_title(const FieldUse &field) {
	return field.label ? std::string(field.label) : field_title(*field.schema);
}

const std::string &choice_title(const FieldChoice &choice) { return choice.label.empty() ? choice.name : choice.label; }

const FieldChoice *choice_of(const std::vector<FieldChoice> &choices, const Value &value) {
	for (const FieldChoice &choice : choices) {
		if (const auto *number = std::get_if<int64_t>(&value); number && *number == choice.value) return &choice;
		if (const auto *text = std::get_if<std::string>(&value); text && strutil::iequals(*text, choice.name)) return &choice;
	}
	return nullptr;
}

const FieldChoice *choice_of(const FieldSchema &field, const Value &value) {
	return choice_of(field.choices, value);
}

bool is_yes_no(const FieldSchema &field) {
	return field.type == FieldType::Integer && !field.flags && field.choices.size() == 2 && field.choices[0].value == 0 &&
	       field.choices[1].value == 1 && field.choices[0].name == "no" && field.choices[1].name == "yes";
}

std::string field_text(const FieldSchema &field, const std::vector<FieldChoice> &choices,
		const Value &value) {
	const auto *number = std::get_if<int64_t>(&value);
	if (field.flags && !choices.empty()) {
		std::string bits;
		for (const FieldChoice &choice : choices)
			if (number && (*number & choice.value) != 0) bits += (bits.empty() ? "" : ", ") + choice_title(choice);
		return bits.empty() ? "none" : bits;
	}
	if (is_yes_no(field)) return number && *number != 0 ? "Yes" : "No";
	if (const FieldChoice *choice = choice_of(choices, value)) return choice_title(*choice);
	if (const auto *real = std::get_if<double>(&value)) {
		char text[32];
		std::snprintf(text, sizeof(text), "%.9g", *real);
		return text;
	}
	if (number) return std::to_string(*number);
	return std::get<std::string>(value);
}

std::string field_text(const FieldSchema &field, const Value &value) {
	return field_text(field, field.choices, value);
}

std::string shown_value(const FieldSchema &field, const std::vector<FieldChoice> &choices,
		const Value &value) {
	const std::string text = field_text(field, choices, value);
	if (!std::holds_alternative<std::string>(value) || choice_of(choices, value)) return text;
	if (text.empty()) return "(empty)";
	const std::string line = text.substr(0, text.find('\n'));
	size_t cut = 80; // a line of a tooltip, never inside a character
	if (line.size() <= cut) return line.size() < text.size() ? line + "..." : line;
	while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80) --cut;
	return line.substr(0, cut) + "...";
}

std::string shown_value(const FieldSchema &field, const Value &value) {
	return shown_value(field, field.choices, value);
}

bool written(const Document &document, const NodeAddress &address, const FieldSchema &field) {
	if (!document.present(address, field.id)) return false;
	if (!is_yes_no(field)) return true;
	Value value;
	const auto *number = document.get(address, field.id, value) ? std::get_if<int64_t>(&value) : nullptr;
	return number && *number != 0;
}

std::string counted(size_t count, const char *noun) {
	return strutil::grouped(count) + " " + noun + (count == 1 ? "" : "s");
}

} // namespace opennova::editor
