#include <editor/blank/blank_factory.h>

#include "blank_makers.h"

namespace opennova::editor {

namespace {

const BlankFactory k_factories[] = {
	// Boot: the string tables and definition files Game_InitSubsystems demands.
	{ "gameerr", AssetKind::Strings, make_blank_empty_strings, "an empty error-message table" },
	{ "gametext", AssetKind::Strings, make_blank_gametext,
	  "the in-game string table with the sections the game reads, empty" },
	{ "vmacros", AssetKind::Strings, make_blank_empty_strings, "an empty voice-macro table" },
	{ "keyhelp", AssetKind::Strings, make_blank_empty_strings, "an empty key-help table" },
	{ "weapon_def", AssetKind::WeaponDefs, make_blank_weapon_def, "a weapon table with no weapons" },
	{ "items_def", AssetKind::ItemDefs, make_blank_items_def, "an item table holding only the Null marker" },
	{ "charattr_def", AssetKind::CharAttrDefs, make_blank_charattr_def,
	  "a character-attribute file with no classes" },
	// Menu: the tables, the stylesheet, the startup screen and the seven fonts.
	{ "game_bin", AssetKind::Strings, make_blank_empty_strings, "an empty menu string table" },
	{ "menu_style", AssetKind::MenuStyle, make_blank_menu_style,
	  "the menu stylesheet naming the fonts and colors the screens use" },
	{ "nw_cdata", AssetKind::StringTableCoo, make_blank_coo, "an empty NovaWorld data table" },
	{ "main_menu", AssetKind::Menu, make_blank_main_menu,
	  "the startup screen: the project's title and an Exit button" },
	{ "menutxt", AssetKind::Strings, make_blank_menutxt, "the menu labels the startup screen shows" },
	{ "font_arial12b", AssetKind::Font, make_blank_font, "the built-in bitmap font" },
	{ "font_arial14n", AssetKind::Font, make_blank_font, "the built-in bitmap font" },
	{ "font_arial14b", AssetKind::Font, make_blank_font, "the built-in bitmap font" },
	{ "font_arial16n", AssetKind::Font, make_blank_font, "the built-in bitmap font" },
	{ "font_arial16b", AssetKind::Font, make_blank_font, "the built-in bitmap font" },
	{ "font_impac22b", AssetKind::Font, make_blank_font, "the built-in bitmap font" },
	{ "font_impac38b", AssetKind::Font, make_blank_font, "the built-in bitmap font" },
	// Mission: the rows the factories can already fill (the rest wait for their writers).
	{ "ammo_def", AssetKind::AmmoDefs, make_blank_ammo_def, "an ammo table holding only the null round" },
};

const size_t k_factory_count = sizeof(k_factories) / sizeof(k_factories[0]);

} // namespace

size_t blank_factory_count() {
	return k_factory_count;
}

const BlankFactory *blank_factory_at(size_t index) {
	return index < k_factory_count ? &k_factories[index] : nullptr;
}

const BlankFactory *find_blank_factory_for_role(std::string_view role) {
	for (const BlankFactory &factory : k_factories) {
		if (role == factory.role) return &factory;
	}
	return nullptr;
}

const BlankFactory *find_blank_factory_for_kind(AssetKind kind) {
	for (const BlankFactory &factory : k_factories) {
		if (factory.kind == kind) return &factory;
	}
	return nullptr;
}

bool make_blank(const BlankRequest &request, AssetKind kind, std::vector<uint8_t> &out,
                Diagnostic &error) {
	const BlankFactory *factory = find_blank_factory_for_role(request.role);
	if (factory == nullptr) factory = find_blank_factory_for_kind(kind);
	if (factory == nullptr) {
		error = make_diagnostic(DiagnosticSeverity::Error, "blank.unavailable",
		                        "The editor cannot create " + request.logical_name +
		                                " yet: no writer exists for this kind of file.",
		                        request.logical_name);
		return false;
	}
	return factory->make(request, out, error);
}

const char *blank_placement_dir(AssetKind kind) {
	switch (kind) {
	case AssetKind::Menu:
	case AssetKind::MenuStyle: return "menus";
	case AssetKind::Strings:
	case AssetKind::StringTableCoo: return "strings";
	case AssetKind::Font: return "fonts";
	case AssetKind::ItemDefs:
	case AssetKind::WeaponDefs:
	case AssetKind::AmmoDefs:
	case AssetKind::HudPosDefs:
	case AssetKind::HudFxDefs:
	case AssetKind::AvatarDefs:
	case AssetKind::SoundProfileDefs:
	case AssetKind::CharAttrDefs:
	case AssetKind::PowerupDefs:
	case AssetKind::OtherDefs: return "defs";
	default: return "";
	}
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
