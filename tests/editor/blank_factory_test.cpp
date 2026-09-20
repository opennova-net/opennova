// Pins the blank factories (ADR 0046 d8): every output parses through its own format
// library and through the runtime's loader, the startup screen compiles through the
// menu frame compiler with every font and string id it names resolvable, text lands
// CRLF, and nothing is embedded that a retail file could supply.
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include <editor/blank/blank_factory.h>
#include <formats/def/def.h>
#include <formats/fnt/fnt.h>
#include <formats/mns/mns_document.h>
#include <formats/mnu/mnu.h>
#include <formats/rtxt/rtxt.h>
#include <runtime/inmatch/charattr_challenge.h>
#include <runtime/menu/menu_frame.h>

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
	for (size_t i = 0; i < blank_factory_count(); ++i) {
		const BlankFactory *f = blank_factory_at(i);
		TEST_EXPECT(f->role != nullptr && f->role[0] != '\0');
		TEST_EXPECT(f->summary != nullptr && f->summary[0] != '\0');
		for (size_t j = 0; j < i; ++j) TEST_EXPECT(std::string(blank_factory_at(j)->role) != f->role);
	}
	BlankRequest request;
	request.logical_name = "walk.bad";
	std::vector<uint8_t> out;
	Diagnostic error;
	TEST_EXPECT(!make_blank(request, AssetKind::Animation, out, error));
	TEST_EXPECT(error.code == "blank.unavailable");
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
	TEST_EXPECT(startup != nullptr);
	TEST_EXPECT(startup->root_window.text_rsrc == "menutxt.BIN");
	const opennova::mnu::Window *exit_button = find_window(startup->root_window, "EXIT");
	TEST_EXPECT(exit_button != nullptr && exit_button->type == opennova::mnu::WindowType::Button);
	TEST_EXPECT(exit_button->actions.empty()); // bound by name, the shell's exit command
	const opennova::mnu::Window *title = find_window(startup->root_window, "TITLE");
	TEST_EXPECT(title != nullptr && title->string_data.value == "Blank & Co");

	// Every string id the screen names resolves through the blank menutxt table.
	const std::vector<uint8_t> menutxt = make("menutxt", "menutxt.bin");
	opennova::rtxt::File menu;
	TEST_EXPECT(opennova::rtxt::parse(menutxt.data(), menutxt.size(), menu, error));
	std::vector<std::string> ids;
	collect_string_ids(startup->root_window, ids);
	TEST_EXPECT(!ids.empty());
	std::map<std::string, std::string> lookup;
	for (const std::string &id : ids) {
		const opennova::rtxt::Entry *entry = menu.find_in_section("Menu", id);
		TEST_EXPECT(entry != nullptr);
		lookup[id] = entry->text;
	}

	// The stylesheet the screen's %VAR% references resolve through.
	const std::vector<uint8_t> style_bytes = make("menu_style", "menu_style.mns");
	const opennova::mns::Document style = opennova::mns::Document::parse(
	        reinterpret_cast<const char *>(style_bytes.data()), style_bytes.size());
	const opennova::mns::EvaluationResult evaluated = style.evaluate();
	TEST_EXPECT(evaluated.success);
	std::map<std::string, std::string> vars(evaluated.sheet.variables.begin(), evaluated.sheet.variables.end());

	// Compile through the runtime's own compiler with the blank font registered under
	// every name the stylesheet can hand out; no interned font or texture may be
	// unresolvable (the compiler would silently fall back).
	const std::vector<uint8_t> font_bytes = make("font_arial16b", "Arial16b.fnt");
	opennova::fnt::fnt_font_t font{};
	TEST_EXPECT(opennova::fnt::fnt_parse(font_bytes.data(), font_bytes.size(), &font) == opennova::fnt::FNT_OK);
	opennova::menu::MenuFrameCompiler compiler;
	compiler.set_style_vars(vars);
	compiler.set_text_lookup(lookup);
	const char *font_names[] = {"Arial12b.fnt", "Arial14n.fnt", "Arial14b.fnt", "Arial16n.fnt",
	                            "Arial16b.fnt", "Impac22b.fnt", "Impac38b.fnt"};
	for (const char *name : font_names) compiler.register_font(name, &font);
	compiler.configure(startup, &font);
	for (const std::string &name : compiler.font_names()) {
		bool known = name.empty();
		for (const char *f : font_names) known = known || compiler.resolve_style_var(name) == f;
		TEST_EXPECT(known);
	}
	TEST_EXPECT(compiler.texture_names().empty());
	TEST_EXPECT(compiler.widget_index("EXIT") >= 0);
	opennova::menu::MenuFrameState state;
	const opennova::menu::MenuDrawList &frame = compiler.compile(state, 1.0f, 1.0f);
	TEST_EXPECT(!frame.font_runs.empty()); // the title and the button label draw text
	opennova::fnt::fnt_free(&font);
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
	TEST_EXPECT(style.serialize() == style_bytes); // lossless: the document's own bytes

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

int main() {
	int failures = 0;
	failures += test_registry_shape();
	failures += test_string_tables();
	failures += test_startup_menu_compiles();
	failures += test_style_and_fonts();
	failures += test_defs_and_coo();
	if (failures == 0) std::printf("editor_blank_factory: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
