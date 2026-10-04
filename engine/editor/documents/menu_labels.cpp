// A menu's parts in a modder's words (menu_labels.h).
#include <editor/documents/menu_labels.h>

#include <cstring>

#include <base/io/strutil.h>
#include <editor/model/field_text.h>
#include <formats/mnu/mnu.h>

namespace opennova::editor {
namespace {

// A text field of the record as it stands ("" for none).
std::string text_of(const Document &document, const NodeAddress &address, const char *field) {
	Value value;
	if (!document.get(address, field, value)) return std::string();
	const auto *text = std::get_if<std::string>(&value);
	return text ? *text : std::string();
}

bool flag_of(const Document &document, const NodeAddress &address, const char *field) {
	Value value;
	if (!document.get(address, field, value)) return false;
	const auto *number = std::get_if<int64_t>(&value);
	return number && *number != 0;
}

// A field's value by its choice's name (the schema's words: "Mouse over" for MOUSEOVER).
std::string choice_words(const Document &document, const NodeAddress &address, const char *field) {
	for (const FieldSchema &schema : document.fields(address.kind)) {
		if (schema.id != field) continue;
		Value value;
		if (!document.get(address, field, value)) return std::string();
		return field_text(schema, value);
	}
	return std::string();
}

// The first line of a text, its ends trimmed.
std::string first_line(const std::string &text) {
	std::string line = text.substr(0, text.find_first_of("\r\n"));
	const size_t start = line.find_first_not_of(" \t");
	const size_t end = line.find_last_not_of(" \t");
	return start == std::string::npos ? std::string() : line.substr(start, end - start + 1);
}

bool is(const std::string &token, const char *name) { return strutil::iequals(token, name); }

// An ACTION by what it does when its window is activated [orig: CUIWidget_HandleScriptedAction @
// 0x6497f0; its WINDOW state switch @ 0x6498f7..0x6499c9, TOGGLE inverting the property the state
// names]: its type's code by the parse's compare (any other token is code 0, which nothing acts on
// [orig: CUIElement_ParseXMLDefinition @ 0x648ee2]); the target is the element text, untrimmed.
std::string action_words(const Document &document, const NodeAddress &address) {
	const std::string type = text_of(document, address, "type");
	const std::string target = first_line(text_of(document, address, "target"));
	if (is(type, "SCREEN")) {
		const std::string file = text_of(document, address, "file");
		if (target.empty()) return "Go to a screen (none named)";
		return "Go to " + target + (file.empty() ? std::string() : " in " + file);
	}
	if (is(type, "WINDOW")) {
		const std::string state = text_of(document, address, "state");
		const bool toggle = flag_of(document, address, "toggle");
		const std::string window = target.empty() ? std::string("a window (none named)") : target;
		if (is(state, "SHOW") || is(state, "HIDE"))
			return (toggle ? std::string("Show or hide ") : is(state, "SHOW") ? std::string("Show ") : std::string("Hide ")) + window;
		if (is(state, "ENABLE") || is(state, "DISABLE"))
			return (toggle ? std::string("Enable or disable ") : is(state, "ENABLE") ? std::string("Enable ") : std::string("Disable ")) +
			       window;
		return "Does nothing to " + window + " (its state is none the game acts on)";
	}
	if (is(type, "POP_SCREEN")) return "Go back";
	if (is(type, "URL")) return target.empty() ? std::string("Open a web page (none named)") : "Open " + target;
	if (is(type, "TAB")) return target.empty() ? std::string("Tab to a control") : "Tab to " + target;
	if (!mnu::known_token(type, mnu::kActionTypes)) return type.empty() ? std::string("Does nothing (no action type)") : "Does nothing (" + type + " is no action type)";
	const std::string words = choice_words(document, address, "type");
	return target.empty() ? words : words + ": " + target;
}

// An APPEARANCE (a window's, a scroll part's, a list's items'): its state, then its look by its type
// (none: it marks the state only).
std::string appearance_words(const Document &document, const NodeAddress &address) {
	std::string state = choice_words(document, address, "state");
	if (state.empty()) state = "Normal";
	const std::string type = text_of(document, address, "type");
	const std::string value = first_line(text_of(document, address, "value"));
	if (type.empty()) return state + " (marks the state only)";
	if (is(type, "IMAGE")) return state + ": image " + value;
	if (is(type, "COLOR")) return state + ": colour " + value;
	const std::string kind = choice_words(document, address, "type");
	return state + ": " + (kind.empty() ? type : kind) + (value.empty() ? std::string() : " " + value);
}

} // namespace

std::string menu_record_label(const Document &document, const NodeAddress &address, const NameSource *) {
	if (!address.child) return std::string();
	const std::string token = document.kind_token(address.kind);
	if (token == "action") return action_words(document, address);
	if (token == "appearance" || token == "shuttle" || token == "scrollup" || token == "scrolldown" ||
	    token == "items.appearance")
		return appearance_words(document, address);
	if (token == "sound") {
		const std::string when = choice_words(document, address, "state");
		const std::string trigger = first_line(text_of(document, address, "trigger"));
		const std::string file = first_line(text_of(document, address, "file"));
		std::string words = (when.empty() ? std::string("Sound") : when) + ": " + (trigger.empty() ? std::string("no sound") : trigger);
		return file.empty() ? words : words + " (" + file + ")";
	}
	if (token == "hotkey") {
		const std::string key = first_line(text_of(document, address, "value"));
		return "Key " + key + (flag_of(document, address, "virtual") ? " (a virtual key)" : "");
	}
	if (token == "datasource") {
		const std::string file = first_line(text_of(document, address, "value"));
		return file.empty() ? std::string() : "Data from " + file;
	}
	if (token == "items.item" || token == "item") {
		const std::string text = first_line(text_of(document, address, "text"));
		return text.empty() ? first_line(text_of(document, address, "value")) : text;
	}
	if (token == "column.header") {
		const std::string text = first_line(text_of(document, address, "text"));
		return text.empty() ? std::string() : "Header: " + text;
	}
	if (token == "column.subst") {
		const std::string value = first_line(text_of(document, address, "value"));
		const std::string file = first_line(text_of(document, address, "file"));
		return value.empty() && file.empty() ? std::string() : value + " shows " + file;
	}
	if (token == "attribute") {
		const std::string name = text_of(document, address, "name");
		const std::string value = text_of(document, address, "value");
		return name.empty() ? std::string() : value.empty() ? name : name + "=" + value;
	}
	return std::string();
}

} // namespace opennova::editor
