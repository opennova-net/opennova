// Pins the blank factories (ADR 0046 d8): every output parses through its own format
// library and through the runtime's loader, the startup screen compiles through the
// menu frame compiler with every font and string id it names resolvable, text lands
// CRLF, a missing texture's placeholder is the game's own checkerboard in the name's
// format, and nothing is embedded that a retail file could supply.
#include <cctype>
#include <cstdio>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

#include <editor/blank/blank_factory.h>
#include <editor/preview/texture_header.h>
#include <formats/dds/dds.h>
#include <formats/def/def.h>
#include <formats/fnt/fnt.h>
#include <formats/mns/mns_document.h>
#include <formats/mnu/mnu.h>
#include <formats/pcx/pcx_io.h>
#include <formats/rtxt/rtxt.h>
#include <formats/tga/tga.h>
#include <runtime/inmatch/charattr_challenge.h>
#include <runtime/menu/menu_assets.h>
#include <runtime/menu/menu_frame.h>
#include <runtime/renderer/material_texture.h>

#include "common/test_expect.h"

using namespace opennova::editor;

static std::vector<uint8_t> make(const char *role, const char *name) {
	BlankRequest request;
	request.logical_name = name;
	request.role = role;
	request.project_title = "Blank & Co";
	std::vector<uint8_t> out;
	Diagnostic error;
	if (!make_blank(request, AssetKind::Unknown, out, error)) {
		std::fprintf(stderr, "make_blank(%s) failed: %s\n", role, error.message.c_str());
		out.clear();
	}
	return out;
}

static std::string text_of(const std::vector<uint8_t> &bytes) {
	return std::string(bytes.begin(), bytes.end());
}

static int is_crlf_text(const std::string &text) {
	for (size_t i = 0; i < text.size(); ++i) {
		if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) return 0;
	}
	return text.find("\r\n") != std::string::npos ? 1 : 0;
}

static void collect_string_ids(const opennova::mnu::Window &window, std::vector<std::string> &ids) {
	if (window.string_data.type == "id" || window.string_data.type == "ID") ids.push_back(window.string_data.value);
	for (const opennova::mnu::Window &child : window.children) collect_string_ids(child, ids);
}

// Every FONT NAME a window tree authors, its parts included.
static void collect_font_names(const opennova::mnu::Window &window, std::vector<std::string> &names) {
	if (!window.font.name.empty()) names.push_back(window.font.name);
	for (const opennova::mnu::WindowPart *part : {&window.list_box, &window.spinup, &window.spindown, &window.scrollbar})
		if (part->present()) collect_font_names(**part, names);
	for (const opennova::mnu::Window &child : window.children) collect_font_names(child, names);
}

static std::string lower(std::string text) {
	for (char &c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return text;
}

static const opennova::mnu::Window *find_window(const opennova::mnu::Window &window, const std::string &name) {
	if (window.name == name) return &window;
	for (const opennova::mnu::Window &child : window.children) {
		if (const opennova::mnu::Window *found = find_window(child, name)) return found;
	}
	return nullptr;
}

static int test_registry_shape() {
	TEST_EXPECT(blank_factory_count() >= 20);
	TEST_EXPECT(blank_factory_at(blank_factory_count()) == nullptr);
	TEST_EXPECT(find_blank_factory_for_role("main_menu") != nullptr);
	TEST_EXPECT(find_blank_factory_for_role("main_menu")->kind == AssetKind::Menu);
	TEST_EXPECT(find_blank_factory_for_role("nope") == nullptr);
	TEST_EXPECT(find_blank_factory_for_kind(AssetKind::Font) != nullptr);
	TEST_EXPECT(find_blank_factory_for_kind(AssetKind::Animation) == nullptr);
	TEST_EXPECT(find_blank_factory_for_role("") == nullptr);
	// A role names one factory; a factory without one is a kind's free-form file.
	for (size_t i = 0; i < blank_factory_count(); ++i) {
		const BlankFactory *f = blank_factory_at(i);
		TEST_EXPECT(f->role != nullptr && (f->role[0] != '\0' || f->free_form));
		TEST_EXPECT(f->summary != nullptr && f->summary[0] != '\0');
		for (size_t j = 0; j < i; ++j)
			if (f->role[0] != '\0') TEST_EXPECT(std::string(blank_factory_at(j)->role) != f->role);
	}
	// Exactly one free-form factory per kind, and the kind lookup returns it: a new
	// menu is not the STARTUP screen.
	std::map<AssetKind, int> free_form;
	for (size_t i = 0; i < blank_factory_count(); ++i) {
		const BlankFactory *f = blank_factory_at(i);
		free_form[f->kind] += f->free_form ? 1 : 0;
	}
	for (const auto &entry : free_form) {
		TEST_EXPECT(entry.second == 1);
		const BlankFactory *f = find_blank_factory_for_kind(entry.first);
		TEST_EXPECT(f != nullptr && f->free_form && f->kind == entry.first);
	}
	TEST_EXPECT(find_blank_factory_for_kind(AssetKind::Menu) != find_blank_factory_for_role("main_menu"));
	TEST_EXPECT(find_blank_factory_for_kind(AssetKind::Strings) != find_blank_factory_for_role("gametext"));
	BlankRequest request;
	request.logical_name = "walk.bad";
	std::vector<uint8_t> out;
	Diagnostic error;
	TEST_EXPECT(!make_blank(request, AssetKind::Animation, out, error));
	TEST_EXPECT(error.code() == "blank.unavailable");
	TEST_EXPECT(std::string(blank_placement_dir(AssetKind::Menu)) == "menus");
	TEST_EXPECT(std::string(blank_placement_dir(AssetKind::Texture)).empty());
	return 0;
}

static int test_string_tables() {
	for (const char *role : {"gameerr", "vmacros", "keyhelp", "game_bin"}) {
		const std::vector<uint8_t> bytes = make(role, "x.bin");
		TEST_EXPECT(!bytes.empty());
		opennova::rtxt::File table;
		std::string error;
		TEST_EXPECT(opennova::rtxt::parse(bytes.data(), bytes.size(), table, error));
		TEST_EXPECT(table.entries.empty());
	}
	const std::vector<uint8_t> gametext = make("gametext", "gametext.bin");
	opennova::rtxt::File table;
	std::string error;
	TEST_EXPECT(opennova::rtxt::parse(gametext.data(), gametext.size(), table, error));
	TEST_EXPECT(table.sections.size() == blank_gametext_sections().size());
	for (size_t i = 0; i < table.sections.size(); ++i) {
		TEST_EXPECT(table.sections[i].name == blank_gametext_sections()[i]);
		TEST_EXPECT(table.sections[i].string_count == 0);
	}
	TEST_EXPECT(table.is_grouped());
	// Re-writing what was parsed gives the same bytes: the table is the writer's own.
	std::vector<uint8_t> again;
	TEST_EXPECT(opennova::rtxt::write(table, again, error) && again == gametext);

	const std::vector<uint8_t> menutxt = make("menutxt", "menutxt.bin");
	opennova::rtxt::File menu;
	TEST_EXPECT(opennova::rtxt::parse(menutxt.data(), menutxt.size(), menu, error));
	TEST_EXPECT(menu.sections.size() == 1 && menu.sections[0].name == "Menu");
	for (const auto &key : blank_menutxt_keys()) TEST_EXPECT(menu.find_in_section("Menu", key.first) != nullptr);
	TEST_EXPECT(menu.get_in_section("Menu", "MM_Exit") == "Exit");
	return 0;
}

static int test_startup_menu_compiles() {
	const std::vector<uint8_t> bytes = make("main_menu", "main.mnu");
	TEST_EXPECT(!bytes.empty());
	TEST_EXPECT(is_crlf_text(text_of(bytes)));
	opennova::mnu::Document doc;
	std::string error;
	TEST_EXPECT(opennova::mnu::parse(bytes.data(), bytes.size(), doc, error));
	const opennova::mnu::Screen *startup = doc.find_screen("STARTUP");
	TEST_EXPECT(startup != nullptr && startup->roots.size() == 1);
	if (!startup || startup->roots.size() != 1) return 1;
	const opennova::mnu::Window &main_window = startup->roots.front();
	const opennova::mnu::Window *exit_button = find_window(main_window, "EXIT");
	TEST_EXPECT(exit_button != nullptr && exit_button->type == opennova::mnu::WindowType::Button);
	TEST_EXPECT(exit_button->actions.empty()); // bound by name, the shell's exit command
	const opennova::mnu::Window *title = find_window(main_window, "TITLE");
	TEST_EXPECT(title != nullptr && title->string_data.value == "Blank & Co");

	// The screen stands on the Required files alone: no text table of its own and no
	// string id, because menutxt.bin is an optional row that a new project does not
	// have, and an id looked up in a missing table draws its raw key.
	TEST_EXPECT(main_window.text_rsrc.empty());
	std::vector<std::string> ids;
	collect_string_ids(main_window, ids);
	TEST_EXPECT(ids.empty());
	TEST_EXPECT(exit_button->string_data.value == "Exit");

	// The stylesheet the screen's %VAR% references resolve through.
	const std::vector<uint8_t> style_bytes = make("menu_style", "menu_style.mns");
	const opennova::mns::Document style = opennova::mns::Document::parse(
	        reinterpret_cast<const char *>(style_bytes.data()), style_bytes.size());
	const opennova::mns::EvaluationResult evaluated = style.evaluate();
	TEST_EXPECT(evaluated.success);
	std::map<std::string, std::string> vars(evaluated.sheet.variables.begin(), evaluated.sheet.variables.end());

	// Compile through the runtime's own compiler with the blank font registered under
	// the seven font files the blank project ships (the font_* factories). A font that
	// does not load draws nothing (there is no default), so every FONT NAME the screen
	// authors must resolve, through the stylesheet and retail's name rule, to one of
	// them; and no texture may be named.
	const std::vector<uint8_t> font_bytes = make("font_arial16b", "Arial16b.fnt");
	opennova::fnt::fnt_font_t font{};
	TEST_EXPECT(opennova::fnt::fnt_parse(font_bytes.data(), font_bytes.size(), &font) == opennova::fnt::FNT_OK);
	opennova::menu::MenuFrameCompiler compiler;
	compiler.set_style_vars(vars);
	const char *font_names[] = {"Arial12b.fnt", "Arial14n.fnt", "Arial14b.fnt", "Arial16n.fnt",
	                            "Arial16b.fnt", "Impac22b.fnt", "Impac38b.fnt"};
	for (const char *name : font_names) compiler.register_font(name, &font);
	compiler.configure(startup);
	std::vector<std::string> authored_fonts;
	collect_font_names(main_window, authored_fonts);
	TEST_EXPECT(!authored_fonts.empty());
	for (const std::string &name : authored_fonts) {
		const std::string file = lower(opennova::menu::menu_font_file(compiler.resolve_style_var(name)));
		bool shipped = false;
		for (const char *f : font_names) shipped = shipped || file == lower(f);
		TEST_EXPECT(shipped);
	}
	TEST_EXPECT(compiler.texture_names().empty());
	TEST_EXPECT(compiler.widget_index("EXIT") >= 0);
	opennova::menu::MenuFrameState state;
	const opennova::menu::MenuDrawList &frame = compiler.compile(state, 1.0f, 1.0f);
	TEST_EXPECT(!frame.font_runs.empty()); // the title and the button label draw text
	opennova::fnt::fnt_free(&font);
	return 0;
}

// A menu of its own (no role): one screen named after the file, upper-case, whose MAIN
// window spans the 800x600 design frame and holds nothing.
static int test_free_form_menu() {
	BlankRequest request;
	request.logical_name = "extra.mnu";
	request.project_title = "Blank & Co";
	std::vector<uint8_t> bytes;
	Diagnostic error;
	TEST_EXPECT(make_blank(request, AssetKind::Menu, bytes, error));
	TEST_EXPECT(is_crlf_text(text_of(bytes)));
	opennova::mnu::Document doc;
	std::string parse_error;
	TEST_EXPECT(opennova::mnu::parse(bytes.data(), bytes.size(), doc, parse_error));
	TEST_EXPECT(doc.screens.size() == 1 && doc.find_screen("EXTRA") != nullptr && doc.find_screen("STARTUP") == nullptr);
	if (doc.screens.size() != 1 || doc.screens[0].roots.size() != 1) return 1;
	const opennova::mnu::Window &main = doc.screens[0].roots.front();
	TEST_EXPECT(main.name == "MAIN" && main.type == opennova::mnu::WindowType::Window && main.children.empty());
	TEST_EXPECT(main.position.has_left && main.position.left == 0 && main.position.has_top && main.position.top == 0);
	TEST_EXPECT(main.position.width() == 800 && main.position.height() == 600);
	TEST_EXPECT(find_window(main, "EXIT") == nullptr && find_window(main, "TITLE") == nullptr);
	TEST_EXPECT(text_of(bytes).find("Blank & Co") == std::string::npos);
	request.logical_name = "Options2.mnu";
	TEST_EXPECT(make_blank(request, AssetKind::Menu, bytes, error));
	opennova::mnu::Document options;
	TEST_EXPECT(opennova::mnu::parse(bytes.data(), bytes.size(), options, parse_error));
	TEST_EXPECT(options.screens.size() == 1 && options.screens[0].name == "OPTIONS2");
	return 0;
}

static int test_style_and_fonts() {
	const std::vector<uint8_t> style_bytes = make("menu_style", "menu_style.mns");
	TEST_EXPECT(is_crlf_text(text_of(style_bytes)));
	const opennova::mns::Document style = opennova::mns::Document::parse(
	        reinterpret_cast<const char *>(style_bytes.data()), style_bytes.size());
	TEST_EXPECT(style.diagnostics().empty());
	const opennova::mns::StyleSheet sheet = style.flatten();
	TEST_EXPECT(sheet.get("DEF_FONTNAME") == "Arial16n.fnt");
	TEST_EXPECT(sheet.get("DEF_FONTNAME_LG") == "Arial16b.fnt");
	TEST_EXPECT(sheet.get("IMPACT_FONTNAME") == "Impac38b.fnt");
	TEST_EXPECT(sheet.has("DEF_TEXT_FG") && sheet.has("TRIM_COLOR") && sheet.has("ITEM_SELECTED_BG"));
	// Eleven: the fonts and colours the screens name (the game reads no variable by name,
	// so nothing else is needed).
	TEST_EXPECT(sheet.variables.size() == 11 && !sheet.has("DEF_IMAGE_DEFAULT_BG"));
	TEST_EXPECT(style.evaluate().success);
	TEST_EXPECT(style.serialize() == style_bytes); // lossless: the document's own bytes
	// brand.mns: a header, no variables, read clean.
	const std::vector<uint8_t> brand_bytes = make("brand_style", "brand.mns");
	TEST_EXPECT(is_crlf_text(text_of(brand_bytes)));
	const opennova::mns::Document brand = opennova::mns::Document::parse(
	        reinterpret_cast<const char *>(brand_bytes.data()), brand_bytes.size());
	TEST_EXPECT(brand.diagnostics().empty() && brand.evaluate().success && brand.flatten().variables.empty());
	TEST_EXPECT(find_blank_factory_for_kind(AssetKind::MenuStyle) == find_blank_factory_for_role("menu_style"));

	const char *roles[] = {"font_arial12b", "font_arial14n", "font_arial14b", "font_arial16n",
	                       "font_arial16b", "font_impac22b", "font_impac38b"};
	std::vector<uint8_t> first;
	for (const char *role : roles) {
		const std::vector<uint8_t> bytes = make(role, "x.fnt");
		TEST_EXPECT(!bytes.empty());
		if (first.empty()) first = bytes;
		TEST_EXPECT(bytes == first); // one glyph set serves every name
		opennova::fnt::fnt_font_t font{};
		TEST_EXPECT(opennova::fnt::fnt_parse(bytes.data(), bytes.size(), &font) == opennova::fnt::FNT_OK);
		TEST_EXPECT(opennova::fnt::fnt_validate(&font) == opennova::fnt::FNT_OK);
		const opennova::fnt::fnt_glyph_t *a = opennova::fnt::fnt_get_glyph(&font, 'A');
		int w = 0, h = 0;
		opennova::fnt::fnt_get_glyph_size(a, &w, &h);
		TEST_EXPECT(w > 0 && h > 0);
		opennova::fnt::fnt_free(&font);
	}
	return 0;
}

static int test_defs_and_coo() {
	using namespace opennova::def;
	const std::vector<uint8_t> items = make("items_def", "items.def");
	TEST_EXPECT(is_crlf_text(text_of(items)));
	DefItemsFile items_file{};
	TEST_EXPECT(def_parse_items_memory(items.data(), items.size(), &items_file) == 0);
	TEST_EXPECT(items_file.count == 1);
	TEST_EXPECT(items_file.entries[0].id == 100000);
	def_free_items(&items_file);

	const std::vector<uint8_t> weapons = make("weapon_def", "weapon.def");
	TEST_EXPECT(is_crlf_text(text_of(weapons)));
	DefWeaponsFile weapons_file{};
	TEST_EXPECT(def_parse_weapons_memory(weapons.data(), weapons.size(), &weapons_file) == 0);
	TEST_EXPECT(weapons_file.count == 0);
	def_free_weapons(&weapons_file);

	const std::vector<uint8_t> ammo = make("ammo_def", "ammo.def");
	TEST_EXPECT(is_crlf_text(text_of(ammo)));
	DefAmmoFile ammo_file{};
	TEST_EXPECT(def_parse_ammo_memory(ammo.data(), ammo.size(), &ammo_file) == 0);
	TEST_EXPECT(ammo_file.count == 1);
	def_free_ammo(&ammo_file);

	const std::vector<uint8_t> charattr = make("charattr_def", "charattr.def");
	TEST_EXPECT(is_crlf_text(text_of(charattr)));
	opennova::inmatch::CharAttrChallengeTable table;
	TEST_EXPECT(opennova::inmatch::parse_charattr_challenge_table(charattr.data(), charattr.size(), table));
	TEST_EXPECT(opennova::inmatch::find_charattr_challenge_row(table, 1) == nullptr); // no classes yet

	const std::vector<uint8_t> coo = make("nw_cdata", "nw_cdata.coo");
	const std::vector<uint8_t> expected = {0, 0, 0, 0, 'R', 'S', 'T', 'R'};
	TEST_EXPECT(coo == expected);
	return 0;
}

// S11h: a missing texture's placeholder is the checkerboard the game draws for a texture it
// cannot load (renderer::missing_material_texture_rgba), its pixels as they are, in the format
// the name's extension asks for: a .tga or an .mdt as a TGA and a .dds as a DDS, each the
// writer's bytes for those pixels and read back as the game's readers read the file, a .pcx as
// the palette image the menu loader's decode gives back as those pixels; every file 128 by 128
// as the editor reads a header. Any other name is refused with the reason in words.
static int test_placeholder_texture() {
	using opennova::menu::MenuTextureFormat;
	const BlankFactory *factory = find_blank_factory_for_kind(AssetKind::Texture);
	TEST_EXPECT(factory != nullptr && factory->free_form && factory->role[0] == '\0' &&
	            std::string(factory->summary).find("the checkerboard the game draws for a missing texture") == 0);
	const std::vector<uint8_t> pixels = opennova::renderer::missing_material_texture_rgba();
	const uint32_t side = opennova::renderer::kMissingMaterialTextureSide;
	TEST_EXPECT(side == 128 && pixels.size() == size_t(side) * side * 4);
	BlankRequest request;
	Diagnostic error;
	std::vector<uint8_t> bytes, expected;
	std::string reason;
	int width = 0, height = 0;
	for (const char *name : {"skin.tga", "SKIN.TGA", "bump.mdt"}) {
		request.logical_name = name;
		TEST_EXPECT(can_make_blank_texture(name, reason) && make_blank(request, AssetKind::Texture, bytes, error));
		TEST_EXPECT(opennova::tga::tga_write_rgba32(pixels.data(), side, side, expected, reason) && bytes == expected);
		TEST_EXPECT(texture_header_size(MenuTextureFormat::Tga, bytes, &width, &height) && width == 128 && height == 128);
		// The block B, G, R, A from the bottom row up, turned upright as the game reads it.
		for (size_t y = 0; y < side; ++y)
			for (size_t x = 0; x < side; ++x) {
				const uint8_t *file = &bytes[opennova::tga::TGA_HEADER_SIZE + ((side - 1 - y) * side + x) * 4];
				const uint8_t *drawn = &pixels[(y * side + x) * 4];
				TEST_EXPECT(file[0] == drawn[2] && file[1] == drawn[1] && file[2] == drawn[0] && file[3] == drawn[3]);
			}
	}
	request.logical_name = "skin.dds";
	TEST_EXPECT(make_blank(request, AssetKind::Texture, bytes, error));
	TEST_EXPECT(opennova::dds::dds_write_a8r8g8b8(pixels.data(), side, side, expected, reason) && bytes == expected);
	TEST_EXPECT(texture_header_size(MenuTextureFormat::Dds, bytes, &width, &height) && width == 128 && height == 128);
	for (size_t i = 0; i < size_t(side) * side; ++i) {
		const uint8_t *file = &bytes[opennova::dds::DDS_HEADER_SIZE + i * 4];
		TEST_EXPECT(file[0] == pixels[i * 4 + 2] && file[1] == pixels[i * 4 + 1] && file[2] == pixels[i * 4] &&
		            file[3] == pixels[i * 4 + 3]);
	}
	request.logical_name = "skin.pcx";
	TEST_EXPECT(make_blank(request, AssetKind::Texture, bytes, error));
	opennova::RgbaImage decoded;
	TEST_EXPECT(opennova::decode_pcx_menu_rgba(bytes.data(), bytes.size(), decoded, reason));
	TEST_EXPECT(decoded.width == 128 && decoded.height == 128 && decoded.pixels == pixels);
	TEST_EXPECT(texture_header_size(MenuTextureFormat::Pcx, bytes, &width, &height) && width == 128 && height == 128);
	// No placeholder for another kind of name (a .png, a chunk container, a name with no extension).
	for (const char *name : {"skin.png", "skin", "chunk.aoc"}) {
		request.logical_name = name;
		TEST_EXPECT(!can_make_blank_texture(name, reason) && !reason.empty());
		TEST_EXPECT(!make_blank(request, AssetKind::Texture, bytes, error) && error.code() == "blank.texture" &&
		            error.message == reason && error.asset == name);
	}
	return 0;
}

int main() {
	int failures = 0;
	failures += test_registry_shape();
	failures += test_string_tables();
	failures += test_startup_menu_compiles();
	failures += test_free_form_menu();
	failures += test_style_and_fonts();
	failures += test_defs_and_coo();
	failures += test_placeholder_texture();
	if (failures == 0) std::printf("editor_blank_factory: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
