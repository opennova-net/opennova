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

#include <editor/assets/asset_kinds.h>
#include <editor/blank/blank_factory.h>
#include <editor/preview/texture_header.h>
#include <formats/dds/dds.h>
#include <formats/def/def.h>
#include <formats/fnt/fnt.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission.h>
#include <formats/mns/mns_document.h>
#include <formats/mnu/mnu.h>
#include <formats/pcx/pcx_io.h>
#include <formats/rtxt/rtxt.h>
#include <formats/tga/tga.h>
#include <formats/tga/tga_read.h>
#include <runtime/inmatch/charattr_challenge.h>
#include <runtime/menu/menu_assets.h>
#include <runtime/menu/menu_frame.h>
#include <runtime/renderer/material_texture.h>
#include <runtime/wac/compiler.h>

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
	// At most one free-form factory per kind, and the kind lookup returns it: a new menu is
	// not the STARTUP screen. A kind whose factories all fill a role has none (an expansion's
	// version text, ADR 0046 S16: a text file is no kind New makes).
	std::map<AssetKind, int> free_form;
	for (size_t i = 0; i < blank_factory_count(); ++i) {
		const BlankFactory *f = blank_factory_at(i);
		free_form[f->kind] += f->free_form ? 1 : 0;
	}
	for (const auto &entry : free_form) {
		TEST_EXPECT(entry.second <= 1);
		const BlankFactory *f = find_blank_factory_for_kind(entry.first);
		TEST_EXPECT(entry.second == 0 ? f == nullptr : f != nullptr && f->free_form && f->kind == entry.first);
	}
	TEST_EXPECT(free_form[AssetKind::Text] == 0 && find_blank_factory_for_role("expansion_version"));
	TEST_EXPECT(find_blank_factory_for_kind(AssetKind::Menu) != find_blank_factory_for_role("main_menu"));
	TEST_EXPECT(find_blank_factory_for_kind(AssetKind::Strings) != find_blank_factory_for_role("gametext"));
	BlankRequest request;
	request.logical_name = "walk.bad";
	std::vector<uint8_t> out;
	Diagnostic error;
	TEST_EXPECT(!make_blank(request, AssetKind::Animation, out, error));
	TEST_EXPECT(error.code() == "blank.unavailable");
	// Where a made file goes and the name New offers are the kind's row's (S13 V3): a kind Files'
	// New lists (its free-form factory has no role) offers a new file's name, and no other kind
	// does.
	TEST_EXPECT(std::string(asset_kind_row(AssetKind::Menu).folder) == "menus");
	TEST_EXPECT(std::string(asset_kind_row(AssetKind::Texture).folder) == "textures");
	TEST_EXPECT(std::string(asset_kind_row(AssetKind::Config).folder).empty());
	for (size_t k = 0; k < kAssetKindCount; ++k) {
		const AssetKind kind = static_cast<AssetKind>(k);
		const BlankFactory *f = find_blank_factory_for_kind(kind);
		const bool listed = f && f->role[0] == '\0';
		TEST_EXPECT(listed == (asset_kind_row(kind).new_name[0] != '\0'));
	}
	TEST_EXPECT(std::string(asset_kind_row(AssetKind::Menu).new_name) == "newmenu.mnu" &&
	            std::string(asset_kind_row(AssetKind::Strings).new_name) == "newtable.bin");
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
	// The one texture it names is the pointer, which the root window's CURSOR names: the original
	// game shows no system pointer (docs/mnu/menu-re.md), so a screen without one has none.
	TEST_EXPECT(main_window.cursor.file == blank_pointer_name() && main_window.cursor.flags == "STANDARD_TRANSPARENT");
	TEST_EXPECT(compiler.texture_names().size() == 1 && compiler.texture_names()[0] == blank_pointer_name());
	TEST_EXPECT(compiler.widget_index("EXIT") >= 0);
	opennova::menu::MenuFrameState state;
	const opennova::menu::MenuDrawList &frame = compiler.compile(state, 1.0f, 1.0f);
	TEST_EXPECT(!frame.font_runs.empty()); // the title and the button label draw text
	// The pointer, loaded at its own size, draws last at the mouse, unscaled.
	const std::vector<uint8_t> pointer = make(kBlankPointerRole, blank_pointer_name());
	opennova::tga::TgaImage image;
	std::string decode_error;
	TEST_EXPECT(opennova::tga::tga_decode_retail(pointer.data(), pointer.size(), image, decode_error));
	compiler.set_texture_size(0, image.width, image.height);
	state.cursor_visible = true;
	state.cursor_x = 100.0f;
	state.cursor_y = 50.0f;
	const opennova::menu::MenuDrawList &pointed = compiler.compile(state, 2.0f, 2.0f);
	TEST_EXPECT(!pointed.quads.empty());
	if (pointed.quads.empty()) return 1;
	const opennova::menu::MenuQuad &last = pointed.quads.back();
	TEST_EXPECT(last.texture == 0 && last.x0 == 100.0f && last.y0 == 50.0f && last.x1 == 132.0f && last.y1 == 82.0f);
	opennova::fnt::fnt_free(&font);
	return 0;
}

// The pointer the blank menus name: a 32-bit TGA in the form the menus' and the splash's readers
// take (the 18-byte header, type 2, 8 alpha bits, the rows from the bottom up), 32 by 32 as the
// shipped one, its tip the top-left pixel (where the game draws the texture from), opaque where
// the arrow is and clear around it; made by its role, by its name as a new texture, and refused
// for a name that is no TGA.
static int test_pointer() {
	const BlankFactory *factory = find_blank_factory_for_role(kBlankPointerRole);
	TEST_EXPECT(factory != nullptr && factory->kind == AssetKind::Texture && !factory->free_form);
	TEST_EXPECT(std::string(blank_pointer_name()) == "newarow1.tga");
	const std::vector<uint8_t> bytes = make(kBlankPointerRole, blank_pointer_name());
	TEST_EXPECT(bytes.size() == opennova::tga::TGA_HEADER_SIZE + 32u * 32u * 4u);
	if (bytes.size() != opennova::tga::TGA_HEADER_SIZE + 32u * 32u * 4u) return 1;
	TEST_EXPECT(bytes[0] == 0 && bytes[1] == 0 && bytes[2] == 2 && bytes[16] == 32 && bytes[17] == 8);
	opennova::tga::TgaImage image;
	std::string reason;
	TEST_EXPECT(opennova::tga::tga_decode_retail(bytes.data(), bytes.size(), image, reason));
	TEST_EXPECT(image.width == 32 && image.height == 32);
	const auto pixel = [&image](int x, int y) { return &image.rgba[(size_t(y) * 32 + size_t(x)) * 4]; };
	TEST_EXPECT(pixel(0, 0)[3] == 0xFF && pixel(0, 0)[0] == 0);                      // the tip, outlined
	TEST_EXPECT(pixel(1, 3)[3] == 0xFF && pixel(1, 3)[0] == 0xFF);                   // the body, white
	TEST_EXPECT(pixel(31, 0)[3] == 0 && pixel(0, 31)[3] == 0 && pixel(31, 31)[3] == 0); // clear around it
	int opaque = 0;
	for (size_t i = 0; i < 32u * 32u; ++i) opaque += image.rgba[i * 4 + 3] == 0xFF ? 1 : 0;
	TEST_EXPECT(opaque > 50 && opaque < 200);
	// Asked as a new texture of its name, the pointer; any other name, the checkerboard.
	BlankRequest request;
	request.logical_name = "NEWAROW1.TGA";
	std::vector<uint8_t> by_name;
	Diagnostic error;
	TEST_EXPECT(find_blank_factory("", request.logical_name, AssetKind::Texture) == factory);
	TEST_EXPECT(make_blank(request, AssetKind::Texture, by_name, error) && by_name == bytes);
	TEST_EXPECT(find_blank_factory("", "other.tga", AssetKind::Texture) == find_blank_factory_for_kind(AssetKind::Texture));
	// The pointer is a TGA: its role refuses another format.
	request.logical_name = "newarow1.dds";
	request.role = kBlankPointerRole;
	TEST_EXPECT(!make_blank(request, AssetKind::Texture, by_name, error) && error.code() == "blank.texture");
	// A menu's blank names it and has it made with it, unless the project builds as an expansion.
	ProjectDocument standalone;
	std::string companion;
	TEST_EXPECT(blank_companion(*find_blank_factory_for_role("main_menu"), standalone, companion) == factory &&
	            companion == blank_pointer_name());
	companion.clear();
	TEST_EXPECT(blank_companion(*find_blank_factory_for_kind(AssetKind::Menu), standalone, companion) == factory &&
	            companion == blank_pointer_name());
	TEST_EXPECT(blank_companion(*find_blank_factory_for_role("gametext"), standalone, companion) == nullptr);
	ProjectDocument expansion;
	expansion.expansion.name = "ptr";
	TEST_EXPECT(blank_companion(*find_blank_factory_for_role("main_menu"), expansion, companion) == nullptr);
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
	TEST_EXPECT(main.cursor.file == blank_pointer_name() && main.cursor.flags == "STANDARD_TRANSPARENT");
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

// ADR 0046 S14: a new mission is its header alone, on the terrain and under the environment asked
// for (a file's name as its picker gives it, with or without its extension), named by its title or
// its file's stem, written through the mission writer (parsed back, made twice byte for byte); a
// name past its slot is refused. Its factory says what it takes (the title, the terrain and the
// environment, the two files required), and blank_values_fit holds a request to that. Its text
// table holds the two keys the mission list reads; a new script is a comment that compiles.
static int test_mission_blanks() {
	const BlankFactory *factory = find_blank_factory_for_kind(AssetKind::Mission);
	TEST_EXPECT(factory && factory->param_count == 3 && std::string(factory->params[0].token) == "title" &&
	            !factory->params[0].required && factory->params[0].reference == ReferenceKind::None &&
	            std::string(factory->params[1].token) == "terrain" && factory->params[1].required &&
	            factory->params[1].reference == ReferenceKind::Terrain &&
	            std::string(factory->params[2].token) == "environment" && factory->params[2].required &&
	            factory->params[2].reference == ReferenceKind::Environment);
	TEST_EXPECT(std::string(asset_kind_row(AssetKind::Mission).folder) == "missions" &&
	            std::string(asset_kind_row(AssetKind::Mission).new_name) == "newmission.bms" &&
	            std::string(asset_kind_row(AssetKind::Script).folder) == "missions");
	if (!factory) return 1;
	BlankRequest request;
	request.logical_name = "first.bms";
	request.values = {{"environment", "DAY"}, {"terrain", "Island.TRN"}, {"title", "The first"}};
	std::string why;
	TEST_EXPECT(blank_values_fit(*factory, request, why) && request.value("terrain") == "Island.TRN" &&
	            request.value("nothing").empty() && blank_mission_title(request) == "The first");
	std::vector<uint8_t> once, twice;
	Diagnostic error;
	TEST_EXPECT(make_blank(request, AssetKind::Mission, once, error) && make_blank(request, AssetKind::Mission, twice, error) &&
	            !once.empty() && once == twice);
	opennova::bms::File file;
	TEST_EXPECT(opennova::bms::parse(once.data(), once.size(), file, why));
	const opennova::mission::MissionInfo info = opennova::mission::mission_info(file);
	TEST_EXPECT(info.mission_name == "The first" && info.terrain == "Island" && info.environment == "DAY" &&
	            info.designer.empty());
	// No title: the file's stem. A value the blank does not take, a required one left out, a name
	// past its slot: refused, in words.
	BlankRequest plain;
	plain.logical_name = "second.bms";
	plain.values = {{"environment", "day.env"}, {"terrain", "island"}};
	std::vector<uint8_t> second;
	TEST_EXPECT(blank_values_fit(*factory, plain, why) && make_blank(plain, AssetKind::Mission, second, error) &&
	            opennova::bms::parse(second.data(), second.size(), file, why));
	TEST_EXPECT(opennova::mission::mission_info(file).mission_name == "second" &&
	            opennova::mission::mission_info(file).environment == "day");
	BlankRequest extra = plain;
	extra.values.push_back({"weather", "rain"});
	TEST_EXPECT(!blank_values_fit(*factory, extra, why) && why.find("takes no value \"weather\"") != std::string::npos &&
	            why.find("title, terrain, environment") != std::string::npos);
	BlankRequest lacking;
	lacking.logical_name = "third.bms";
	lacking.values = {{"terrain", "island"}};
	TEST_EXPECT(!blank_values_fit(*factory, lacking, why) && why == "third.bms needs its environment.");
	BlankRequest long_name = plain;
	long_name.values.push_back({"title", std::string(40, 'x')});
	std::vector<uint8_t> none;
	TEST_EXPECT(!make_blank(long_name, AssetKind::Mission, none, error) && none.empty() && error.code() == "blank.mission");

	// The text table beside it: [Info] TITLE and an empty BRIEFING.
	const BlankFactory *text = find_blank_factory_for_role(kBlankMissionTextRole);
	TEST_EXPECT(text && text->kind == AssetKind::Strings && !text->free_form);
	BlankRequest table_request;
	table_request.logical_name = "first.bin";
	table_request.role = kBlankMissionTextRole;
	table_request.values = {{"title", "The first"}};
	std::vector<uint8_t> table_bytes;
	TEST_EXPECT(make_blank(table_request, AssetKind::Strings, table_bytes, error));
	opennova::rtxt::File table;
	TEST_EXPECT(opennova::rtxt::parse(table_bytes.data(), table_bytes.size(), table, why) && table.sections.size() == 1 &&
	            table.sections[0].name == "Info" && table.entries.size() == 2 && table.entries[0].key == "TITLE" &&
	            table.entries[0].text == "The first" && table.entries[1].key == "BRIEFING" && table.entries[1].text.empty());

	// A script: a comment naming it, CR LF, which the compiler takes with nothing to say.
	BlankRequest script;
	script.logical_name = "patrol.wac";
	std::vector<uint8_t> script_bytes;
	TEST_EXPECT(make_blank(script, AssetKind::Script, script_bytes, error) && text_of(script_bytes) == "// patrol.wac\r\n");
	const opennova::wac::Program program = opennova::wac::compile_source(text_of(script_bytes), opennova::wac::CompileEnv());
	TEST_EXPECT(program.ok() && program.diagnostics.empty() && program.event_count == 0);
	return 0;
}

int main() {
	int failures = 0;
	failures += test_registry_shape();
	failures += test_mission_blanks();
	failures += test_string_tables();
	failures += test_startup_menu_compiles();
	failures += test_free_form_menu();
	failures += test_style_and_fonts();
	failures += test_defs_and_coo();
	failures += test_placeholder_texture();
	failures += test_pointer();
	if (failures == 0) std::printf("editor_blank_factory: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
