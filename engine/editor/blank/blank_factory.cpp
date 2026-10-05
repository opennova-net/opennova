#include <editor/blank/blank_factory.h>

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

// A terrain made from images (S20): its images (files on disk, or of the project) and the importer's
// options, the new_terrain request's values.
const BlankParam k_terrain_params[] = {
	{ "heightmap", "Heightmap (1024 x 1024 PNG or .raw)", ReferenceKind::None, true },
	{ "colormap", "Colour map (1024 x 1024 image)", ReferenceKind::None, true },
	{ "detail", "Detail (optional, power-of-two image)", ReferenceKind::None, false },
	{ "tiles", "Tile set (optional, sides x64)", ReferenceKind::None, false },
	{ "top", "Height of white (world units, 127.5)", ReferenceKind::None, false },
	{ "water", "Water level (world units, 0 none)", ReferenceKind::None, false },
	{ "layout", "Layout (island or tiled)", ReferenceKind::None, false },
};

const BlankFactory k_factories[] = {
	// Boot: the string tables and definition files Game_InitSubsystems demands.
	{ "gameerr", AssetKind::Strings, make_blank_empty_strings, "an empty error-message table", false },
	{ "gametext", AssetKind::Strings, make_blank_gametext,
	  "the in-game string table with the sections the game reads, empty", false },
	{ "vmacros", AssetKind::Strings, make_blank_empty_strings, "an empty voice-macro table", false },
	{ "keyhelp", AssetKind::Strings, make_blank_empty_strings, "an empty key-help table", false },
	{ "weapon_def", AssetKind::WeaponDefs, make_blank_weapon_def, "a weapon table with no weapons", true },
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
	  "the startup screen: the project's title and an Exit button", false },
	{ "menutxt", AssetKind::Strings, make_blank_menutxt,
	  "a menu label table holding the common navigation labels", false },
	{ "font_arial12b", AssetKind::Font, make_blank_font, "the built-in bitmap font", false },
	{ "font_arial14n", AssetKind::Font, make_blank_font, "the built-in bitmap font", false },
	{ "font_arial14b", AssetKind::Font, make_blank_font, "the built-in bitmap font", false },
	{ "font_arial16n", AssetKind::Font, make_blank_font, "the built-in bitmap font", false },
	{ "font_arial16b", AssetKind::Font, make_blank_font, "the built-in bitmap font", false },
	{ "font_impac22b", AssetKind::Font, make_blank_font, "the built-in bitmap font", false },
	{ "font_impac38b", AssetKind::Font, make_blank_font, "the built-in bitmap font", false },
	// Mission: the rows the factories can already fill (the rest wait for their writers).
	{ "ammo_def", AssetKind::AmmoDefs, make_blank_ammo_def, "an ammo table holding only the null round", true },
	// An expansion's own (ADR 0046 S16): its text table, naming it in the Mods list, and its version text.
	{ "expansion_table", AssetKind::Strings, make_blank_expansion_table,
	  "the expansion's text table: its name in the Mods list (the project's title) and an empty description", false },
	{ "expansion_version", AssetKind::Text, make_blank_expansion_version,
	  "the expansion's version text (the project's title), whose checksum a joiner must match", false },
	// Free-form: a new file of a kind whose required files are all specific (Create
	// menu, Create table, a font of another name, a missing texture's placeholder).
	{ "", AssetKind::Strings, make_blank_empty_strings, "an empty string table", true },
	{ "", AssetKind::Menu, make_blank_menu, "a menu with one screen named after the file, empty", true },
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
	// S20: an environment a mission can be made under (a terrain made from images has none).
	{ "", AssetKind::Environment, make_blank_environment,
	  "a daytime environment: noon light, sky and fog colours through the day, the stock cloud maps", true },
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

const BlankParam *new_file_params(AssetKind kind, size_t &count, bool *offered) {
	count = 0;
	if (offered) *offered = false;
	if (kind == AssetKind::Terrain) {
		count = sizeof(k_terrain_params) / sizeof(k_terrain_params[0]);
		if (offered) *offered = true;
		return k_terrain_params;
	}
	// The kind's free-form factory of no role: what Files' New lists under its kind's label.
	for (const BlankFactory &factory : k_factories)
		if (factory.kind == kind && factory.free_form && factory.role[0] == '\0') {
			if (offered) *offered = true;
			count = factory.param_count;
			return factory.params;
		}
	return nullptr;
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

bool make_blank(const BlankRequest &request, AssetKind kind, std::vector<uint8_t> &out,
                Diagnostic &error) {
	const BlankFactory *factory = find_blank_factory_for_role(request.role);
	if (factory == nullptr) factory = find_blank_factory_for_kind(kind);
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
