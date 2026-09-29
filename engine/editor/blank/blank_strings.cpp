#include "blank_makers.h"

#include <formats/rtxt/rtxt.h>
#include <runtime/hud/game_text_lookup.h>

namespace opennova::editor {

using namespace opennova::rtxt;

namespace {

bool write_table(const File &table, const BlankRequest &request, std::vector<uint8_t> &out,
                 Diagnostic &error) {
	std::string write_error;
	if (!write(table, out, write_error)) {
		out.clear();
		error = make_diagnostic(DiagnosticSeverity::Error, "blank.strings", write_error,
		                        request.logical_name);
		return false;
	}
	return true;
}

// Append one section; its keys are grouped contiguously (the engine derives an
// entry's index by accumulating the preceding sections' string_counts
// [orig: TextResource_FindEntryBySectionAndKey @ 0x75D250]).
void add_section(File &table, const std::string &name,
                 const std::vector<std::pair<std::string, std::string>> &rows) {
	const uint32_t index = static_cast<uint32_t>(table.sections.size());
	table.sections.push_back({name, static_cast<uint32_t>(rows.size())});
	for (const auto &row : rows) {
		Entry entry;
		entry.key = row.first;
		entry.text = row.second;
		entry.section_index = index;
		table.entries.push_back(entry);
	}
}

} // namespace

const std::vector<std::string> &blank_gametext_sections() {
	// The sections the game reads from gametext.bin by name
	// (godot/game/strings/strings.gd SECTION_*; the HUD text tables).
	static const std::vector<std::string> sections = {
		hud::kGameTextOverlays, hud::kGameTextWepDes, hud::kGameTextWPNames,
		hud::kGameTextCannedMsg, hud::kGameTextClient, hud::kGameTextLoadingText,
	};
	return sections;
}

const std::vector<std::pair<std::string, std::string>> &blank_menutxt_keys() {
	// A starting label table for the screens a modder adds; the shell resolves menu
	// tokens through menutxt's "Menu" section first. menutxt.bin is an optional row, so
	// it is made only when asked for by role, and the blank startup screen never
	// depends on it (its labels are literal text).
	static const std::vector<std::pair<std::string, std::string>> keys = {
		{"MM_Exit", "Exit"},
		{"NAV_ACCEPT", "Accept"},
		{"NAV_BACK", "Back"},
	};
	return keys;
}

bool make_blank_empty_strings(const BlankRequest &request, std::vector<uint8_t> &out,
                              Diagnostic &error) {
	return write_table(File(), request, out, error);
}

bool make_blank_gametext(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &error) {
	File table;
	for (const std::string &section : blank_gametext_sections()) add_section(table, section, {});
	return write_table(table, request, out, error);
}

bool make_blank_menutxt(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &error) {
	File table;
	add_section(table, "Menu", blank_menutxt_keys());
	return write_table(table, request, out, error);
}

} // namespace opennova::editor
