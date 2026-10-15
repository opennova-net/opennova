#include "blank_makers.h"

#include <base/io/strutil.h>
#include <base/vfs/vfs.h>
#include <formats/rtxt/rtxt.h>
#include <runtime/hud/game_text_lookup.h>
#include <runtime/mission/mission_catalog.h>

namespace opennova::editor {

using namespace opennova::rtxt;

namespace {

bool write_table(const File &table, const BlankRequest &request, std::vector<uint8_t> &out,
                 Diagnostic &error) {
	std::string write_error;
	if (!write(table, out, write_error)) {
		out.clear();
		error = make_finding(CoreFinding::BlankStrings, DiagnosticSeverity::Error, write_error,
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

// A mission's text table (<mission>.bin): the two keys the mission list reads of it, its title
// and its briefing (mission_catalog's [Info] TITLE and BRIEFING) [orig:
// MissionList_ScanAndBuildFromFiles @ 0x563170], the briefing empty.
bool make_blank_mission_text(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &error) {
	File table;
	add_section(table, mission_catalog::kTextInfoSection,
	            {{mission_catalog::kTextTitleKey, blank_mission_title(request)}, {mission_catalog::kTextBriefingKey, std::string()}});
	return write_table(table, request, out, error);
}

// An expansion's text table (<n>.bin, ADR 0046 S16): the two keys the Mods list reads of it, its name
// and its description [orig: Expansion_ScanAndRegister: TextResource_FindEntryBySectionAndKey
// ("exp_info", "EXP_NAME") @ 0x4a4578, ("exp_info", "EXP_DESC") @ 0x4a45ef], the name the project's
// title, the description empty. The game's table overrides its strings by section and key too: the
// modder adds those sections here.
bool make_blank_expansion_table(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &error) {
	File table;
	std::string title = request.project_title.empty() ? request.logical_name : request.project_title;
	// The Mods list copies EXP_NAME into a 64-byte name with no bound, a longer one running over the
	// record's directory [orig: Expansion_ScanAndRegister @ 0x4a4598]: the title is cut to 63 bytes, at a
	// character's start, so the table made is one the build takes (build.expansion.exp_name).
	title.resize(strutil::utf8_cut(title, kExpansionRecordNameBytes - 1));
	add_section(table, kExpansionInfoSection, {{kExpansionNameKey, title}, {kExpansionDescriptionKey, std::string()}});
	return write_table(table, request, out, error);
}

// An expansion's version text (version.txt): its only reader takes the checksum of its bytes
// [orig: Expansion_LoadAssets @ 0x4a4858..0x4a4885]; the project's title on one line, as the shipped
// one names its expansion.
bool make_blank_expansion_version(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &) {
	const std::string title = request.project_title.empty() ? request.logical_name : request.project_title;
	blank_text_to_bytes(title + "\n", out);
	return true;
}

} // namespace opennova::editor
