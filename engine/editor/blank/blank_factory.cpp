#include <editor/blank/blank_factory.h>

#include <base/io/strutil.h>

#include "blank_makers.h"

namespace opennova::editor {

namespace {

// What a new mission asks: the name its header carries (its file's stem when left out), and the
// terrain and the environment it loads, each a file of the project (a mission with neither loads
// nothing).
const BlankParam k_mission_params[] = {
	{ "title", "Title", ReferenceKind::None, false },
	{ "terrain", "Terrain", ReferenceKind::Terrain, true },
	{ "environment", "Environment", ReferenceKind::Environment, true },
};
const BlankParam k_mission_text_params[] = {
	{ "title", "Title", ReferenceKind::None, false },
};

const BlankFactory k_factories[] = {
	// Boot: the string tables and definition files Game_InitSubsystems demands.
	{ "gameerr", AssetKind::Strings, make_blank_empty_strings, "an empty error-message table", false },
	{ "gametext", AssetKind::Strings, make_blank_gametext,
	  "the in-game string table with the sections the game reads, empty", false },
	{ "vmacros", AssetKind::Strings, make_blank_empty_strings, "an empty voice-macro table", false },
	{ "keyhelp", AssetKind::Strings, make_blank_empty_strings, "an empty key-help table", false },
	{ "weapon_def", AssetKind::WeaponDefs, make_blank_weapon_def, "a weapon table with no weapons", true },
	{ "sndprof_def", AssetKind::SoundProfileDefs, make_blank_sound_profiles,
	  "the sound profiles: one \"default\" profile, every slot silent", true },
	{ "items_def", AssetKind::ItemDefs, make_blank_items_def, "an item table holding only the Null marker", true },
	{ "charattr_def", AssetKind::CharAttrDefs, make_blank_charattr_def,
	  "a character-attribute file with no classes", true },
	// Menu: the tables, the stylesheet, the startup screen and the seven fonts.
	{ "game_bin", AssetKind::Strings, make_blank_empty_strings, "an empty menu string table", false },
	{ "menu_style", AssetKind::MenuStyle, make_blank_menu_style,
	  "the menu stylesheet naming the fonts and colors the screens use", true },
	{ "brand_style", AssetKind::MenuStyle, make_blank_brand_style,
	  "a brand stylesheet with no variables yet, read after the menu stylesheet", false },
	{ "nw_cdata", AssetKind::StringTableCoo, make_blank_coo, "an empty NovaWorld data table", true },
	{ "main_menu", AssetKind::Menu, make_blank_main_menu,
	  "the startup screen: the project's title, an Exit button and the game's mouse pointer", false },
	// The pointer every blank menu names (kBlankPointerRole), made with the menu.
	{ kBlankPointerRole, AssetKind::Texture, make_blank_pointer,
	  "the game's mouse pointer, a white arrow outlined in black, which the menus name", false },
	{ "menutxt", AssetKind::Strings, make_blank_menutxt,
	  "a menu label table holding the common navigation labels", false },
	{ "font_arial12b", AssetKind::Font, make_blank_font, "the built-in bitmap font", false },
	{ "font_arial14n", AssetKind::Font, make_blank_font, "the built-in bitmap font", false },
	{ "font_arial14b", AssetKind::Font, make_blank_font, "the built-in bitmap font", false },
	{ "font_arial16n", AssetKind::Font, make_blank_font, "the built-in bitmap font", false },
	{ "font_arial16b", AssetKind::Font, make_blank_font, "the built-in bitmap font", false },
	{ "font_impac22b", AssetKind::Font, make_blank_font, "the built-in bitmap font", false },
	{ "font_impac38b", AssetKind::Font, make_blank_font, "the built-in bitmap font", false },
	// Mission: what a mission's start and its screens read by name (docs/required-resources.md).
	{ "ammo_def", AssetKind::AmmoDefs, make_blank_ammo_def, "an ammo table holding only the null round", true },
	{ "powerup_def", AssetKind::PowerupDefs, make_blank_powerup_def, "a powerup table with no powerups yet", true },
	{ "cmap_menu", AssetKind::Menu, make_blank_cmap_menu,
	  "the command map screen (CMAP) with its Close button, on Esc and on V, the key that opens it", false },
	{ "game_menu", AssetKind::Menu, make_blank_game_menu,
	  "the in-mission menu (INGAME): Resume, on Esc, and Leave Mission with its question", false },
	{ "weapon_menu", AssetKind::Menu, make_blank_weapon_menu,
	  "the armory screen (WEAPON) with its Cancel button, on Esc; no weapon slots yet", false },
	{ "vehicle_menu", AssetKind::Menu, make_blank_vehicle_menu,
	  "the vehicle loadout screen (VEHICLE) with its Cancel button, on Esc; no weapon list yet", false },
	{ "stat_menu", AssetKind::Menu, make_blank_stat_menu,
	  "the end-of-round screen (STAT): Leave Mission, on Esc, with its question; no results table yet", false },
	{ "death_menu", AssetKind::Menu, make_blank_death_menu,
	  "the deploy screen (DEATH): the spawn list the game fills, and Leave Mission with its question", false },
	{ "mp_menu", AssetKind::Menu, make_blank_mp_menu,
	  "the multiplayer screen the game comes back to (NW_MULTI_PLAYER) with its Back button, on Esc", false },
	{ "font_arials18", AssetKind::Font, make_blank_font, "the built-in bitmap font", false },
	{ "font_arial22", AssetKind::Font, make_blank_font, "the built-in bitmap font", false },
	{ "font_couri20b", AssetKind::Font, make_blank_font, "the built-in bitmap font", false },
	{ "game_wac", AssetKind::Script, make_blank_script,
	  "an empty script, compiled ahead of server.wac and every mission's own", false },
	{ "server_wac", AssetKind::Script, make_blank_script,
	  "an empty script, compiled after game.wac and ahead of every mission's own", false },
	{ "loadscrn_pcx", AssetKind::Texture, make_blank_loading_screen,
	  "the checkerboard the game draws for a missing texture, 800 by 600: the loading screen's image", false },
	{ "monogram_tga", AssetKind::Texture, make_blank_monogram,
	  "the checkerboard the game draws for a missing texture, 512 by 256: the boards' watermark", false },
	{ "boxtile_tga", AssetKind::Texture, make_blank_boxtile,
	  "the checkerboard the game draws for a missing texture, 256 by 256: the boards' fill", false },
	{ "border_tga", AssetKind::Texture, make_blank_texture,
	  "the checkerboard the game draws for a missing texture, 128 by 128: the boards' border pieces", false },
	// The mission's sound banks, each slot of the bank loop that loads what it finds (the sound lane):
	// an empty bank, which the editor's bank document fills.
	{ "game_lwf", AssetKind::SoundBank, make_blank_sound_bank, "the global sound bank, no set yet", false },
	{ "gamelocl_lwf", AssetKind::SoundBank, make_blank_sound_bank, "the localized sound bank, no set yet", false },
	{ "game2_lwf", AssetKind::SoundBank, make_blank_sound_bank, "a further global sound bank, no set yet", false },
	{ "game3_lwf", AssetKind::SoundBank, make_blank_sound_bank, "a further global sound bank, no set yet", false },
	{ "expansion_lwf", AssetKind::SoundBank, make_blank_sound_bank, "the expansion's sound bank, no set yet", false },
	{ "expansion_locl_lwf", AssetKind::SoundBank, make_blank_sound_bank,
	  "the expansion's localized sound bank, no set yet", false },
	// An expansion's own (ADR 0046 S16): its text table, naming it in the Mods list, and its version text.
	{ "expansion_table", AssetKind::Strings, make_blank_expansion_table,
	  "the expansion's text table: its name in the Mods list (the project's title) and an empty description", false },
	{ "expansion_version", AssetKind::Text, make_blank_expansion_version,
	  "the expansion's version text (the project's title), whose checksum a joiner must match", false },
	// Free-form: a new file of a kind whose required files are all specific (Create
	// menu, Create table, a font of another name, a missing texture's placeholder).
	{ "", AssetKind::Strings, make_blank_empty_strings, "an empty string table", true },
	{ "", AssetKind::Menu, make_blank_menu,
	  "a menu with one screen named after the file, empty but for the game's mouse pointer", true },
	{ "", AssetKind::Font, make_blank_font, "the built-in bitmap font", true },
	{ "", AssetKind::Texture, make_blank_texture,
	  "the checkerboard the game draws for a missing texture, 128 by 128 gray squares", true },
	// S14: a mission on the terrain and under the environment chosen, with no entity yet; the text
	// table made beside it (its title, an empty briefing); a script.
	{ "", AssetKind::Mission, make_blank_mission, "an empty mission on the terrain and under the environment chosen",
	  true, k_mission_params, sizeof(k_mission_params) / sizeof(k_mission_params[0]) },
	{ kBlankMissionTextRole, AssetKind::Strings, make_blank_mission_text,
	  "a mission's text table: its title and an empty briefing", false, k_mission_text_params,
	  sizeof(k_mission_text_params) / sizeof(k_mission_text_params[0]) },
	{ "", AssetKind::Script, make_blank_script, "an empty script", true },
	{ "", AssetKind::SoundBank, make_blank_sound_bank, "a sound bank with no set yet", true },
};

const size_t k_factory_count = sizeof(k_factories) / sizeof(k_factories[0]);

} // namespace

const std::string &BlankRequest::value(std::string_view token) const {
	static const std::string none;
	for (const auto &entry : values)
		if (entry.first == token) return entry.second;
	return none;
}

bool blank_values_fit(const BlankFactory &factory, const BlankRequest &request, std::string &why) {
	std::string takes;
	for (size_t i = 0; i < factory.param_count; ++i) takes += std::string(i ? ", " : "") + factory.params[i].token;
	for (const auto &entry : request.values) {
		bool known = false;
		for (size_t i = 0; i < factory.param_count; ++i) known = known || entry.first == factory.params[i].token;
		if (known) continue;
		why = request.logical_name + " takes no value \"" + entry.first + "\"" +
		      (takes.empty() ? std::string(" (it takes none).") : " (it takes " + takes + ").");
		return false;
	}
	for (size_t i = 0; i < factory.param_count; ++i) {
		const BlankParam &param = factory.params[i];
		if (!param.required || !request.value(param.token).empty()) continue;
		why = request.logical_name + " needs its " + param.token + ".";
		return false;
	}
	return true;
}

size_t blank_factory_count() {
	return k_factory_count;
}

const BlankFactory *blank_factory_at(size_t index) {
	return index < k_factory_count ? &k_factories[index] : nullptr;
}

const BlankFactory *find_blank_factory_for_role(std::string_view role) {
	if (role.empty()) return nullptr;
	for (const BlankFactory &factory : k_factories) {
		if (role == factory.role) return &factory;
	}
	return nullptr;
}

const BlankFactory *find_blank_factory_for_kind(AssetKind kind) {
	for (const BlankFactory &factory : k_factories) {
		if (factory.kind == kind && factory.free_form) return &factory;
	}
	return nullptr;
}

const BlankFactory *find_blank_factory(std::string_view role, const std::string &logical_name, AssetKind kind) {
	if (const BlankFactory *factory = find_blank_factory_for_role(role)) return factory;
	if (kind == AssetKind::Texture && strutil::iequals(logical_name, blank_pointer_name()))
		return find_blank_factory_for_role(kBlankPointerRole);
	return find_blank_factory_for_kind(kind);
}

const BlankFactory *blank_companion(const BlankFactory &factory, const ProjectDocument &doc, std::string &name) {
	if (factory.kind != AssetKind::Menu || !doc.expansion.standalone()) return nullptr;
	name = blank_pointer_name();
	return find_blank_factory_for_role(kBlankPointerRole);
}

bool make_blank(const BlankRequest &request, AssetKind kind, std::vector<uint8_t> &out,
                Diagnostic &error) {
	const BlankFactory *factory = find_blank_factory(request.role, request.logical_name, kind);
	if (factory == nullptr) {
		error = make_finding(CoreFinding::BlankUnavailable, DiagnosticSeverity::Error,
		                     "The editor cannot create " + request.logical_name +
		                             " yet: no writer exists for this kind of file.",
		                     request.logical_name);
		return false;
	}
	return factory->make(request, out, error);
}

std::string blank_crlf(const std::string &text) {
	std::string out;
	out.reserve(text.size() + text.size() / 16);
	for (size_t i = 0; i < text.size(); ++i) {
		const char c = text[i];
		if (c == '\r') continue; // normalize any authored CR first
		if (c == '\n') out += "\r\n";
		else out.push_back(c);
	}
	return out;
}

void blank_text_to_bytes(const std::string &text, std::vector<uint8_t> &out) {
	const std::string crlf = blank_crlf(text);
	out.assign(crlf.begin(), crlf.end());
}

} // namespace opennova::editor
