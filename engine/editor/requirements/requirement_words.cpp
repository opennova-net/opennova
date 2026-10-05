#include <editor/requirements/requirement_words.h>

#include <iterator>

#include <base/gameprofile/required_resources.h>

namespace opennova::editor {

namespace {

// Each row says the manifest row of its role in plain words (base/gameprofile/required_resources.cpp,
// whose failure and citation are the witness: requirement_witness). In the manifest's order.
constexpr RequirementWords kWords[] = {
	// --- the boot [orig: Game_Run @ 0x4a7fb0 -> Game_InitSubsystems @ 0x4a6cd0] ---
	{ "fgn2_bin", "The game loads the standard effects (its .ptu particle files) beside the .ptl ones: with this file there, the German set (.ptg) instead. It checks only whether the file is there." },
	{ "cc_bin", "The game runs with no country code." },
	{ "gameerr", "The game shows an error dialog as it starts, then starts anyway." },
	{ "gametext", "The game shows \"Unable to load game strings\" and exits." },
	{ "vmacros", "The game shows \"Unable to load voice macro strings\" and exits." },
	{ "keyhelp", "The game shows \"Unable to load keyboard map strings\" and exits." },
	{ "weapon_def", "The game starts with no weapons: its weapon list holds only \"None\"." },
	{ "avatars_def", "The game starts without avatar definitions." },
	{ "sndprof_def", "A mission can hang or crash: with no profile, every item takes its sounds from memory the game never cleared. One empty \"default\" profile is enough." },
	{ "items_def", "The game starts with no item definitions." },
	{ "charattr_def", "The game logs \"Could not load charattr definitions\" and goes on without them." },
	{ "loading_pcx", "The loading screen shows no picture." },
	// --- the main menu [orig: Menu_InitShellResources @ 0x552500] ---
	{ "game_bin", "The menu texts this table holds are missing." },
	{ "prolog_bik", "The prologue video does not play." },
	{ "intro_bik", "The intro video does not play." },
	{ "menumus_sbf", "The menus play no music." },
	{ "menumus_bin", "The menus play no music." },
	{ "menu_style", "The menus draw unstyled: their fonts and colours come from this file." },
	{ "brand_style", "Nothing changes: the game ships none; one would restyle the menus after menu_style.mns." },
	{ "main_bik", "The menu shows no video there." },
	{ "header_bik", "The menu shows no video there." },
	{ "footer_bik", "The menu shows no video there." },
	{ "nw_cdata", "The game goes on without this string table: the texts it holds are missing." },
	{ "main_menu", "The main menu never appears: the game stops on a blank screen, with no message." },
	{ "menutxt", "The menus fall back on the game's built-in texts." },
	{ "font_arial12b", "Text in this font draws nothing." },
	{ "font_arial14n", "Text in this font draws nothing." },
	{ "font_arial14b", "Text in this font draws nothing." },
	{ "font_arial16n", "Text in this font draws nothing." },
	{ "font_arial16b", "Text in this font draws nothing." },
	{ "font_impac22b", "Text in this font draws nothing." },
	{ "font_impac38b", "Text in this font draws nothing." },
	{ "menu_lwf", "The menus go without the sounds of this bank." },
	{ "pi_idle_bad", "The player preview stands without its idle animation." },
	{ "dt1rst_bad", "The player preview has no rest pose." },
	{ "hwmcube_dds", "The player preview has no environment reflection." },
	// --- a mission's start [orig: Game_StartMission @ 0x524360] ---
	{ "failsafe_bad", "A clip that does not load plays nothing (the shipped game has none either)." },
	{ "gamelocl_lwf", "Missions go without the sounds of this bank." },
	{ "game_lwf", "Missions go without the sounds of this bank." },
	{ "game3_lwf", "Missions go without the sounds of this bank." },
	{ "game2_lwf", "Missions go without the sounds of this bank." },
	{ "ammo_def", "Missions start with no ammunition types." },
	{ "powerup_def", "The game logs \"Unable to load powerup.def\" and goes on without it." },
	{ "medmssn_bin", "A mission with no text table of its own shows no texts." },
	{ "game_wac", "Missions run without the game's shared script." },
	{ "server_wac", "Missions run without the server script." },
	{ "gamemus_sbf", "Multiplayer missions play no music." },
	{ "gamemus_bin", "Multiplayer missions play no music." },
	{ "loadscrn_pcx", "The mission loading screen shows no picture." },
	{ "font_arials18", "The mission loading screen's text in this font draws nothing." },
	{ "font_arial22", "The mission loading screen's text in this font draws nothing." },
	{ "cmap_menu", "In a mission, the command map screen is missing." },
	{ "game_menu", "In a mission, the in-game menu screen is missing." },
	{ "weapon_menu", "In a mission, the armory's weapon screen is missing." },
	{ "vehicle_menu", "In a mission, the vehicle screen is missing." },
	{ "stat_menu", "The end-of-round statistics screen is missing." },
	{ "death_menu", "The death screen is missing." },
	{ "mp_menu", "The multiplayer screen is missing." },
	{ "hudfx_def", "The HUD goes without the effects this file defines." },
	{ "hudpos_def", "The HUD uses its default positions." },
	{ "monogram_tga", "A mission's screens draw without this texture." },
	{ "boxtile_tga", "A mission's screens draw without this texture." },
	{ "border_tga", "A mission's screens draw without this texture." },
	{ "upl_3di", "The sky draws without this model." },
	{ "font_couri20b", "Text in this font draws nothing." },
};

constexpr bool sentence(const char *text) {
	if (!*text || !(text[0] >= 'A' && text[0] <= 'Z')) return false;
	const char *end = text;
	while (*end) ++end;
	return end[-1] == '.';
}

constexpr bool words_well_formed() {
	for (const RequirementWords &row : kWords)
		if (!*row.role || !sentence(row.without)) return false;
	return true;
}
static_assert(words_well_formed(), "every row a role and a sentence, a capital first and a full stop last");

} // namespace

const RequirementWords *requirement_words(const std::string &role) {
	for (const RequirementWords &row : kWords)
		if (role == row.role) return &row;
	return nullptr;
}

std::string requirement_without(const std::string &role) {
	const RequirementWords *row = requirement_words(role);
	return row ? row->without : std::string();
}

std::string requirement_witness(const std::string &role) {
	const gameprofile::RequiredResource *row = gameprofile::gameprofile_required_resource_by_role(role.c_str());
	if (!row) return std::string();
	std::string out = row->failure ? row->failure : "";
	if (row->orig && *row->orig) out += (out.empty() ? "" : " ") + std::string(row->orig);
	return out;
}

size_t requirement_words_count() { return std::size(kWords); }

const RequirementWords &requirement_words_at(size_t index) { return kWords[index]; }

} // namespace opennova::editor
