// S12 D9 (ADR 0046 S12): Rename everywhere, over a real session, one test per kind of name. The
// plan (PreviewRename, the view's rename_preview) lists the definition and every use that reaches
// exactly it; RenameSymbol rewrites them on disk. A menu screen used from another menu (the other
// menu's own screen of the name, and its use, stay); a style variable brand.mns defines again
// (brand.mns's and its uses renamed, menu_style.mns's shadowed one alone); a string key two
// sections define, one used by a def and the other by a menu (each renamed with its own use
// only), the files with unsaved edits saved first through the unsaved prompt; a weapon's name
// (a name the catalog has already, or one too long for the field, refused); an ammo's name; an
// item id a mission names (refused, naming the mission); a powerup row's name, the item that binds it
// following and an item naming none found missing; a model's user point (the same-named
// point of another model, and its use, stay). The D9 review: a saved use behind an unsaved edit
// asks to save first; a style variable used as a menu's font; a name a document refuses writes
// nothing; an item id compared as the number it is. The second review: the files written together
// (a later one the system will not replace puts the earlier ones back); a name its definition
// would hold in another form refused; an open document that would not write refused, and a dirty
// menu whose saved use reaches another definition not asked to save.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/reference_queries.h>
#include <editor/graph/rename_transaction.h>
#include <editor/project/project_files.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/session_json.h>
#include <editor/session/view/session_view.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
namespace fs = std::filesystem;

namespace {

using editor_test::NoProcess;

// A new project with its missing files made, the session and its root.
struct Project {
	editor_test::TempProjectDir dir;
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session;
	explicit Project(const char *name) : dir(name), session(platform, preferences) {
		editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Rename"));
		editor_test::create_missing_files(session);
	}
	const SessionView &view() const { return session.view(); }
	const AssetGraph &graph() const { return *session.view().findings.graph; }
	std::string root() const { return session.view().project.root; }
	// A project file's path, by its name.
	std::string path(const char *name) const {
		const AssetEntry *entry = view().project.scan->find(name);
		return entry ? entry->relative_path : std::string();
	}
	bool write(const std::string &relative, const std::string &text) { return editor_test::write_text(root() + "/" + relative, text); }
	std::string read(const std::string &relative) const {
		std::ifstream in(root() + "/" + relative, std::ios::binary);
		std::stringstream buffer;
		buffer << in.rdbuf();
		return buffer.str();
	}
	// A Rescan, run to its end (an operation, S13 A3).
	void rescan() {
		editor_test::handle_to_end(session, request::rescan());
	}
	// The definition of a name of `kind` in `file` (by its path).
	const GraphSymbol *defined(ReferenceKind kind, const std::string &name, const std::string &file) const {
		for (const GraphSymbol *symbol : graph().symbols_named(kind, name))
			if (symbol->file == file) return symbol;
		return nullptr;
	}
	EditorRequest request(EditorRequestKind kind, const GraphSymbol &symbol, const std::string &name) const {
		return kind == EditorRequestKind::RenameSymbol
		               ? request::rename_symbol(symbol.file, symbol.locator, symbol.field, name)
		               : request::preview_rename(symbol.file, symbol.locator, symbol.field, name);
	}
	// The plan of renaming `symbol` to `name`, as the view carries it.
	const DialogsView::RenamePreview &preview(const GraphSymbol &symbol, const std::string &name) {
		editor_test::handle_to_end(session, request(EditorRequestKind::PreviewRename, symbol, name));
		return view().dialogs.rename_preview;
	}
	// The rename committed (its operation run to its end): whether it went through.
	bool rename(const GraphSymbol &symbol, const std::string &name) {
		return editor_test::handle_to_end(session, request(EditorRequestKind::RenameSymbol, symbol, name)).done();
	}
};

const GraphEdge *edge_to(const AssetGraph &graph, const std::string &source, ReferenceKind kind, const std::string &value) {
	for (const GraphEdge *edge : graph.references_of(source))
		if (edge->source == source && edge->kind == kind && edge->value == value) return edge;
	return nullptr;
}

size_t sites_in(const DialogsView::RenamePreview &plan, const std::string &file) {
	return size_t(std::count_if(plan.sites->begin(), plan.sites->end(), [&](const RenameSite &site) { return site.file == file; }));
}

bool refused(const DialogsView::RenamePreview &plan, const char *code, const std::string &file) {
	return std::any_of(plan.refusals.begin(), plan.refusals.end(),
	                   [&](const Diagnostic &d) { return d.code() == code && d.asset == file; });
}

std::string window(const char *type, const char *name, const std::string &body = std::string()) {
	return std::string("<WINDOW TYPE=\"") + type + "\" NAME=\"" + name + "\">\r\n" +
	       "<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>100</RIGHT><BOTTOM>20</BOTTOM></POSITION>\r\n" + body + "</WINDOW>\r\n";
}

std::string screen(const char *name, const std::string &body) {
	return std::string("<SCREEN>\r\n<NAME>") + name + "</NAME>\r\n" + body + "</SCREEN>\r\n";
}

std::string go(const char *file, const char *screen_name) {
	return std::string("<ACTION TYPE=\"SCREEN\" FILE=\"") + file + "\">" + screen_name + "</ACTION>\r\n";
}

} // namespace

// A menu screen used from another menu: a.mnu's HOME, its own ACTION and b.mnu's ACTION naming
// it are renamed; b.mnu's own HOME screen and the ACTION reaching it stay.
static int test_menu_screen() {
	Project project("opennova_rename_screen");
	TEST_EXPECT(project.write("menus/a.mnu", screen("HOME", window("STATIC", "PANEL", go("a.mnu", "HOME"))) +
	                                                 screen("AWAY", window("STATIC", "BOARD"))));
	TEST_EXPECT(project.write("menus/b.mnu", screen("BEE", window("BUTTON", "GO", go("a.mnu", "HOME") + go("b.mnu", "HOME"))) +
	                                                 screen("HOME", window("STATIC", "X"))));
	project.rescan();
	const GraphSymbol *home = project.defined(ReferenceKind::MenuScreen, "HOME", "menus/a.mnu");
	TEST_EXPECT(home && !home->inert);
	if (!home) return 1;
	const DialogsView::RenamePreview &plan = project.preview(*home, "START");
	TEST_EXPECT(plan.symbol && plan.kind == ReferenceKind::MenuScreen && plan.old_name == "HOME" && plan.refusals.empty());
	TEST_EXPECT(plan.sites->size() == 3 && sites_in(plan, "menus/a.mnu") == 2 && sites_in(plan, "menus/b.mnu") == 1);
	for (const RenameSite &site : *plan.sites) TEST_EXPECT(site.before == "HOME" && site.after == "START");
	TEST_EXPECT(project.rename(*home, "START"));
	const AssetGraph &graph = project.graph();
	TEST_EXPECT(project.defined(ReferenceKind::MenuScreen, "START", "menus/a.mnu") &&
	            project.defined(ReferenceKind::MenuScreen, "HOME", "menus/b.mnu"));
	const GraphEdge *renamed = edge_to(graph, "menus/b.mnu", ReferenceKind::MenuScreen, "START");
	const GraphEdge *kept = edge_to(graph, "menus/b.mnu", ReferenceKind::MenuScreen, "HOME");
	TEST_EXPECT(renamed && renamed->scope == "A.MNU" && graph.resolve(*renamed) == ReferenceStatus::Present);
	TEST_EXPECT(kept && kept->scope == "B.MNU" && graph.resolve(*kept) == ReferenceStatus::Present);
	TEST_EXPECT(edge_to(graph, "menus/a.mnu", ReferenceKind::MenuScreen, "START"));
	// A name a.mnu has already: refused, naming a.mnu; b.mnu's screen may take it (another scope).
	const GraphSymbol *away = project.defined(ReferenceKind::MenuScreen, "AWAY", "menus/a.mnu");
	TEST_EXPECT(away && refused(project.preview(*away, "start"), "rename.exists", "menus/a.mnu"));
	const GraphSymbol *bee_home = project.defined(ReferenceKind::MenuScreen, "HOME", "menus/b.mnu");
	TEST_EXPECT(bee_home && project.preview(*bee_home, "START").refusals.empty());
	return 0;
}

// A style variable brand.mns defines again: brand.mns's is the one the game reads, and it and
// its uses are renamed (the %NAME% typed is its NAME); menu_style.mns's, shadowed, has no uses.
static int test_style_variable() {
	Project project("opennova_rename_style");
	const std::string sheet = project.path("menu_style.mns");
	TEST_EXPECT(!sheet.empty());
	TEST_EXPECT(project.write(sheet, project.read(sheet) + "SHADOWED FF000000\r\n"));
	TEST_EXPECT(project.write("menus/brand.mns", "SHADOWED FF102030\r\n"));
	TEST_EXPECT(project.write("menus/c.mnu", screen("C", window("STATIC", "W",
	                                                                 "<APPEARANCE STATE=\"DEFAULT\" TYPE=\"COLOR\">%SHADOWED%"
	                                                                 "</APPEARANCE>\r\n"))));
	project.rescan();
	const GraphSymbol *brand = project.defined(ReferenceKind::StyleVar, "SHADOWED", "menus/brand.mns");
	const GraphSymbol *plain = project.defined(ReferenceKind::StyleVar, "SHADOWED", sheet);
	TEST_EXPECT(brand && !brand->inert && plain && plain->inert);
	if (!brand || !plain) return 1;
	const DialogsView::RenamePreview &shadowed = project.preview(*plain, "OLDSHADE");
	TEST_EXPECT(shadowed.refusals.empty() && shadowed.sites->size() == 1 && (*shadowed.sites)[0].file == sheet);
	const DialogsView::RenamePreview &plan = project.preview(*brand, "%BRANDED%");
	TEST_EXPECT(plan.refusals.empty() && plan.new_name == "BRANDED" && plan.sites->size() == 2);
	TEST_EXPECT(plan.sites->size() == 2 && (*plan.sites)[0].after == "BRANDED" && (*plan.sites)[1].file == "menus/c.mnu" &&
	            (*plan.sites)[1].before == "%SHADOWED%" && (*plan.sites)[1].after == "%BRANDED%");
	TEST_EXPECT(project.rename(*brand, "BRANDED"));
	const AssetGraph &graph = project.graph();
	const GraphSymbol *binding = graph.style_binding("BRANDED");
	TEST_EXPECT(binding && binding->file == "menus/brand.mns");
	TEST_EXPECT(project.defined(ReferenceKind::StyleVar, "SHADOWED", sheet) != nullptr);
	const GraphEdge *use = edge_to(graph, "menus/c.mnu", ReferenceKind::StyleVar, "%BRANDED%");
	TEST_EXPECT(use && graph.resolve(*use) == ReferenceStatus::Present);
	TEST_EXPECT(project.read("menus/brand.mns").find("BRANDED") != std::string::npos);
	return 0;
}

// A menu's text that is one %NAME% (a STRING of no id type) shows the variable's value: a use of
// the variable, renamed with it (S13 D4's review: no edge read it, and a rename left it stale).
static int test_style_variable_as_text() {
	Project project("opennova_rename_style_text");
	const std::string sheet = project.path("menu_style.mns");
	TEST_EXPECT(!sheet.empty());
	TEST_EXPECT(project.write(sheet, project.read(sheet) + "GREETING Hello\r\n"));
	TEST_EXPECT(project.write("menus/t.mnu", screen("T", window("STATIC", "W", "<STRING>%GREETING%</STRING>\r\n"))));
	project.rescan();
	const GraphSymbol *greeting = project.defined(ReferenceKind::StyleVar, "GREETING", sheet);
	TEST_EXPECT(greeting != nullptr);
	if (!greeting) return 1;
	const GraphEdge *use = edge_to(project.graph(), "menus/t.mnu", ReferenceKind::StyleVar, "%GREETING%");
	TEST_EXPECT(use && use->through == ReferenceKind::MenuText);
	const DialogsView::RenamePreview &plan = project.preview(*greeting, "SALUTE");
	TEST_EXPECT(plan.refusals.empty() && sites_in(plan, "menus/t.mnu") == 1);
	TEST_EXPECT(project.rename(*greeting, "SALUTE"));
	TEST_EXPECT(project.read("menus/t.mnu").find("<STRING>%SALUTE%</STRING>") != std::string::npos);
	return 0;
}

// Any text of a menu that is one %NAME% is a use of the variable, the game expanding the whole
// text before its parse (S13 D4's second review): an ACTION's URL, and a window's NAME with the
// WINDOW target naming it, each renamed with the variable, so the target still finds the window.
static int test_style_variable_in_any_text() {
	Project project("opennova_rename_style_any_text");
	const std::string sheet = project.path("menu_style.mns");
	TEST_EXPECT(!sheet.empty());
	TEST_EXPECT(project.write(sheet, project.read(sheet) + "HOME_URL http://x/\r\nPANEL PANEL_A\r\n"));
	TEST_EXPECT(project.write("menus/u.mnu",
	                          screen("U", window("BUTTON", "WEB", "<ACTION TYPE=\"URL\">%HOME_URL%</ACTION>\r\n") +
	                                              window("STATIC", "%PANEL%") +
	                                              window("BUTTON", "OPEN",
	                                                     "<ACTION TYPE=\"WINDOW\" STATE=\"SHOW\">%PANEL%</ACTION>\r\n"))));
	project.rescan();
	const GraphSymbol *url = project.defined(ReferenceKind::StyleVar, "HOME_URL", sheet);
	TEST_EXPECT(url != nullptr);
	if (!url) return 1;
	TEST_EXPECT(project.rename(*url, "SITE"));
	std::string text = project.read("menus/u.mnu");
	TEST_EXPECT(text.find("%SITE%") != std::string::npos && text.find("%HOME_URL%") == std::string::npos);
	const GraphSymbol *panel = project.defined(ReferenceKind::StyleVar, "PANEL", sheet);
	TEST_EXPECT(panel != nullptr);
	if (!panel) return 1;
	const DialogsView::RenamePreview &plan = project.preview(*panel, "PANE");
	TEST_EXPECT(plan.refusals.empty() && sites_in(plan, "menus/u.mnu") == 2);
	TEST_EXPECT(project.rename(*panel, "PANE"));
	text = project.read("menus/u.mnu");
	size_t renamed = 0;
	for (size_t at = text.find("%PANE%"); at != std::string::npos; at = text.find("%PANE%", at + 1)) ++renamed;
	TEST_EXPECT(renamed == 2 && text.find("%PANEL%") == std::string::npos);
	return 0;
}

// A string key two sections of gametext.bin define: WepDes's is the weapon's loadout label, the
// "menu" section's the menu's string. Each renamed with its own use alone; the table's unsaved
// edits are saved first, through the unsaved prompt.
static int test_string_key() {
	Project project("opennova_rename_string");
	const std::string weapons = project.path("weapon.def");
	TEST_EXPECT(project.write(weapons, "weapon \"GUN_T\"\nloadout_menu_textid SHARED_KEY\nend\n"));
	TEST_EXPECT(project.write("menus/d.mnu", screen("D", window("STATIC", "LABEL",
	                                                                 "<TEXT_RSRC>gametext.bin</TEXT_RSRC>\r\n"
	                                                                 "<STRING TYPE=\"ID\">SHARED_KEY</STRING>\r\n"))));
	project.rescan();
	const std::string table_path = project.path("gametext.bin");
	editor_test::handle_to_end(project.session, request::open_document(table_path));
	Document *table = project.session.document_for(table_path);
	TEST_EXPECT(table != nullptr);
	if (!table) return 1;
	const auto add_key = [&](NodeId section, const char *key) {
		EditorRequest add = request::edit_record(table->path(), Edit());
		add.edits[0].operation = EditOperation::Add;
		add.edits[0].address = {section, table->kind_from_name("string"), 0};
		editor_test::handle_to_end(project.session, add);
		EditorRequest set = request::edit_record(table->path(), Edit());
		set.edits[0].address = {section, table->kind_from_name("string"), table->last_added()};
		set.edits[0].field = "key";
		set.edits[0].value = std::string(key);
		editor_test::handle_to_end(project.session, set);
	};
	NodeId wepdes = 0;
	for (const auto &row : table->rows())
		if (row->name() == "WepDes") wepdes = row->id;
	TEST_EXPECT(wepdes != 0);
	add_key(wepdes, "SHARED_KEY");
	{
		EditorRequest add = request::edit_record(table->path(), Edit());
		add.edits[0].operation = EditOperation::Add;
		add.edits[0].address = {0, table->kind_from_name("section"), 0};
		editor_test::handle_to_end(project.session, add);
		const NodeId section = table->last_added();
		EditorRequest name = request::edit_record(table->path(), Edit());
		name.edits[0].address = {section, table->kind_from_name("section"), 0};
		name.edits[0].field = "name";
		name.edits[0].value = std::string("menu");
		editor_test::handle_to_end(project.session, name);
		add_key(section, "SHARED_KEY");
	}
	const GraphSymbol *weapon_key = nullptr, *menu_key = nullptr;
	for (const GraphSymbol *symbol : project.graph().symbols_named(ReferenceKind::TextId, "SHARED_KEY"))
		(symbol->scope == "GAMETEXT.BIN/WepDes" ? weapon_key : menu_key) = symbol;
	TEST_EXPECT(weapon_key && menu_key && menu_key->scope == "GAMETEXT.BIN/menu");
	if (!weapon_key || !menu_key) return 1;
	const DialogsView::RenamePreview &plan = project.preview(*weapon_key, "WEP_RENAMED");
	TEST_EXPECT(plan.refusals.empty() && plan.sites->size() == 2 && sites_in(plan, weapons) == 1 &&
	            sites_in(plan, "menus/d.mnu") == 0);
	// The table has unsaved edits: the rename waits on the prompt, whose Save writes them first.
	const EditorRequest weapon_rename = project.request(EditorRequestKind::RenameSymbol, *weapon_key, "WEP_RENAMED");
	editor_test::handle_to_end(project.session, weapon_rename);
	TEST_EXPECT(project.session.outcome().unsaved_prompt &&
			project.view().dialogs.unsaved_prompt.open &&
			project.view().dialogs.unsaved_prompt.action == EditorRequestKind::RenameSymbol &&
			!project.view().dialogs.unsaved_prompt.can_discard &&
			project.view().dialogs.unsaved_prompt.files == std::vector<std::string>{ table_path });
	EditorRequest save = request::resolve_unsaved(UnsavedChoice::Save);
	TEST_EXPECT(editor_test::handle_to_end(project.session, save).done());
	const AssetGraph &graph = project.graph();
	const GraphEdge *label = edge_to(graph, weapons, ReferenceKind::TextId, "WEP_RENAMED");
	TEST_EXPECT(label && graph.resolve(*label) == ReferenceStatus::Present);
	const GraphEdge *string = edge_to(graph, "menus/d.mnu", ReferenceKind::TextId, "SHARED_KEY");
	TEST_EXPECT(string && graph.resolve(*string) == ReferenceStatus::Present);
	// The menu's own key, with its use alone.
	menu_key = nullptr;
	for (const GraphSymbol *symbol : graph.symbols_named(ReferenceKind::TextId, "SHARED_KEY")) menu_key = symbol;
	TEST_EXPECT(menu_key && menu_key->scope == "GAMETEXT.BIN/menu");
	if (!menu_key) return 1;
	TEST_EXPECT(project.rename(*menu_key, "MENU_RENAMED"));
	const GraphEdge *renamed = edge_to(project.graph(), "menus/d.mnu", ReferenceKind::TextId, "MENU_RENAMED");
	TEST_EXPECT(renamed && project.graph().resolve(*renamed) == ReferenceStatus::Present);
	TEST_EXPECT(edge_to(project.graph(), weapons, ReferenceKind::TextId, "WEP_RENAMED"));
	return 0;
}

// A weapon's name: its item's primary weapon follows; a name another weapon has, and one longer
// than the field holds, are refused naming weapon.def.
static int test_weapon_name() {
	Project project("opennova_rename_weapon");
	const std::string weapons = project.path("weapon.def"), items = project.path("items.def");
	TEST_EXPECT(project.write(weapons, "weapon \"GUN_A\"\nend\nweapon \"GUN_C\"\nend\n"));
	TEST_EXPECT(project.write(items, "begin \"Carrier\"\nid 100300\ntype vehicle\nprimary_weapon GUN_A\nend\n"));
	project.rescan();
	const GraphSymbol *gun = project.defined(ReferenceKind::Weapon, "GUN_A", weapons);
	TEST_EXPECT(gun != nullptr);
	if (!gun) return 1;
	TEST_EXPECT(refused(project.preview(*gun, "GUN_C"), "rename.exists", weapons));
	TEST_EXPECT(refused(project.preview(*gun, std::string(200, 'G')), "rename.too_long", weapons));
	TEST_EXPECT(refused(project.preview(*gun, ""), "rename.name", weapons));
	const DialogsView::RenamePreview &plan = project.preview(*gun, "GUN_B");
	TEST_EXPECT(plan.refusals.empty() && plan.sites->size() == 2 && sites_in(plan, items) == 1);
	TEST_EXPECT(project.rename(*gun, "GUN_B"));
	TEST_EXPECT(project.defined(ReferenceKind::Weapon, "GUN_B", weapons));
	const GraphEdge *primary = edge_to(project.graph(), items, ReferenceKind::Weapon, "GUN_B");
	TEST_EXPECT(primary && project.graph().resolve(*primary) == ReferenceStatus::Present);
	TEST_EXPECT(project.read(items).find("GUN_A") == std::string::npos);
	return 0;
}

// An ammo's name: the weapon's round follows.
static int test_ammo_name() {
	Project project("opennova_rename_ammo");
	const std::string weapons = project.path("weapon.def");
	TEST_EXPECT(project.write("defs/ammo.def", "ammo AMMO_A\nend\n"));
	TEST_EXPECT(project.write(weapons, "weapon \"GUN_A\"\nround_type AMMO_A\nend\n"));
	project.rescan();
	const std::string ammo = project.path("ammo.def");
	const GraphSymbol *round = project.defined(ReferenceKind::Ammo, "AMMO_A", ammo);
	TEST_EXPECT(round != nullptr);
	if (!round) return 1;
	const DialogsView::RenamePreview &plan = project.preview(*round, "AMMO_B");
	TEST_EXPECT(plan.refusals.empty() && plan.sites->size() == 2 && sites_in(plan, weapons) == 1);
	TEST_EXPECT(project.rename(*round, "AMMO_B"));
	const GraphEdge *used = edge_to(project.graph(), weapons, ReferenceKind::Ammo, "AMMO_B");
	TEST_EXPECT(used && project.graph().resolve(*used) == ReferenceStatus::Present);
	return 0;
}

// A powerup row's name (S13 D10): the item binding it follows; an item naming a row powerup.def lacks
// is a reference.missing finding (the game destroys such an item as the mission starts).
static int test_powerup_name() {
	Project project("opennova_rename_powerup");
	const std::string items = project.path("items.def");
	TEST_EXPECT(project.write("defs/powerup.def", "powerup \"PU_MED\"\r\nhp -1\r\nend\r\n"));
	TEST_EXPECT(project.write(items, "begin \"Med Pack\"\nid 100400\ntype powerup\npowerupdef PU_MED\nend\n"
	                                 "begin \"Stray Pack\"\nid 100401\ntype powerup\npowerupdef PU_NONE\nend\n"));
	project.rescan();
	const std::string powerups = project.path("powerup.def");
	const GraphSymbol *med = project.defined(ReferenceKind::Powerup, "PU_MED", powerups);
	TEST_EXPECT(!powerups.empty() && med != nullptr);
	if (!med) return 1;
	bool missing = false;
	for (const Diagnostic &d : project.view().findings.diagnostics)
		missing = missing || (d.code() == "reference.missing" && d.asset == items && d.field == "powerup_def" &&
		                      d.message.find("PU_NONE") != std::string::npos);
	TEST_EXPECT(missing);
	const DialogsView::RenamePreview &plan = project.preview(*med, "PU_HEAL");
	TEST_EXPECT(plan.refusals.empty() && plan.sites->size() == 2 && sites_in(plan, items) == 1);
	TEST_EXPECT(project.rename(*med, "PU_HEAL"));
	const GraphEdge *used = edge_to(project.graph(), items, ReferenceKind::Powerup, "PU_HEAL");
	TEST_EXPECT(used && project.graph().resolve(*used) == ReferenceStatus::Present);
	TEST_EXPECT(project.read(items).find("PU_MED") == std::string::npos &&
	            project.read(powerups).find("PU_HEAL") != std::string::npos);
	return 0;
}

// An item id a mission places: the editor cannot rewrite the mission, so the rename is refused,
// the finding naming it, and nothing is written.
static int test_item_id_refused() {
	Project project("opennova_rename_item");
	const std::string items = project.path("items.def");
	TEST_EXPECT(project.write(items, "begin \"Placed\"\nid 100300\ntype building\nend\n"));
	{
		opennova::bms::File mission;
		opennova::mission::make_default(mission);
		opennova::mission::add_entity(mission, opennova::mission::EntityKind::Item, 100300, {});
		std::vector<uint8_t> bytes;
		std::string error;
		TEST_EXPECT(opennova::bms::write(mission, bytes, error));
		TEST_EXPECT(editor_test::write_bytes(project.root() + "/missions/place.bms", bytes));
	}
	project.rescan();
	const GraphSymbol *item = project.defined(ReferenceKind::Item, "100300", items);
	TEST_EXPECT(item != nullptr);
	if (!item) return 1;
	const DialogsView::RenamePreview &plan = project.preview(*item, "100301");
	TEST_EXPECT(refused(plan, "rename.site", "missions/place.bms"));
	TEST_EXPECT(refused(project.preview(*item, "not a number"), "rename.name", items));
	const std::string before = project.read(items);
	TEST_EXPECT(!project.rename(*item, "100301"));
	TEST_EXPECT(project.read(items) == before);
	// What the rename's operation came to: its plan, made once the validation it joined had
	// ended, refused (S13 A3).
	bool reported = false;
	for (const Diagnostic &d : project.session.view().activity.last_operation.findings)
		reported = reported || (d.code() == "rename.site" && d.asset == "missions/place.bms");
	TEST_EXPECT(reported);
	return 0;
}

// A model's user point: the item slot naming it on that model follows; another model's point of
// the same name, and the slot naming that one, stay.
static int test_user_point() {
	Project project("opennova_rename_point");
	const std::string items = project.path("items.def");
	const fs::path fixture = fs::path(__FILE__).parent_path().parent_path().parent_path() / "fixtures" / "threedi" / "synth" / "armory.3di";
	std::error_code ec;
	fs::create_directories(fs::path(project.root()) / "models", ec);
	fs::copy_file(fixture, fs::path(project.root()) / "models" / "armory.3di", ec);
	fs::copy_file(fixture, fs::path(project.root()) / "models" / "other.3di", ec);
	TEST_EXPECT(!ec);
	// armory.3di's user points are "Armory" and "Ground".
	TEST_EXPECT(project.write(items, "begin \"Armory Item\"\nid 100100\ntype building\ngraphic armory\n"
	                                 "particlefx Effect_x ARMORY\nend\n"
	                                 "begin \"Other Item\"\nid 100101\ntype building\ngraphic other\n"
	                                 "particlefx Effect_x ARMORY\nend\n"));
	project.rescan();
	const GraphSymbol *point = project.defined(ReferenceKind::UserPoint, "Armory", "models/armory.3di");
	TEST_EXPECT(point && point->scope == "ARMORY.3DI");
	if (!point) return 1;
	const DialogsView::RenamePreview &plan = project.preview(*point, "Muzzle");
	TEST_EXPECT(plan.refusals.empty() && plan.sites->size() == 2 && sites_in(plan, items) == 1 &&
	            sites_in(plan, "models/other.3di") == 0);
	TEST_EXPECT(plan.sites->size() == 2 && (*plan.sites)[1].before == "ARMORY" && (*plan.sites)[1].after == "Muzzle");
	TEST_EXPECT(project.rename(*point, "Muzzle"));
	const AssetGraph &graph = project.graph();
	TEST_EXPECT(project.defined(ReferenceKind::UserPoint, "Muzzle", "models/armory.3di") &&
	            project.defined(ReferenceKind::UserPoint, "Armory", "models/other.3di"));
	const GraphEdge *renamed = edge_to(graph, items, ReferenceKind::UserPoint, "Muzzle");
	const GraphEdge *kept = edge_to(graph, items, ReferenceKind::UserPoint, "ARMORY");
	TEST_EXPECT(renamed && renamed->scope == "ARMORY.3DI" && graph.resolve(*renamed) == ReferenceStatus::Present);
	TEST_EXPECT(kept && kept->scope == "OTHER.3DI" && graph.resolve(*kept) == ReferenceStatus::Present);
	return 0;
}

// A saved use an unsaved edit hides (D9 review): items.def names GUN_A on disk, its open document
// GUN_C. The rename waits on the unsaved prompt over items.def (the commit reads the saved files);
// saved, the rename is planned again, and GUN_C, now on disk, is left as it is.
static int test_saved_use_behind_an_edit() {
	Project project("opennova_rename_saved_use");
	const std::string weapons = project.path("weapon.def"), items = project.path("items.def");
	TEST_EXPECT(project.write(weapons, "weapon \"GUN_A\"\nend\nweapon \"GUN_C\"\nend\n"));
	TEST_EXPECT(project.write(items, "begin \"Carrier\"\nid 100300\ntype vehicle\nprimary_weapon GUN_A\nend\n"));
	project.rescan();
	editor_test::handle_to_end(project.session, request::open_document(items));
	Document *table = project.session.document_for(items);
	NodeAddress carrier;
	TEST_EXPECT(table && find_definition(AssetGraph(), *table, "100300", carrier));
	if (!table) return 1;
	EditorRequest edit = request::edit_record(items, Edit());
	edit.edits[0].address = carrier;
	edit.edits[0].field = "primary_weapon";
	edit.edits[0].value = std::string("GUN_C");
	editor_test::handle_to_end(project.session, edit);
	const GraphSymbol *gun = project.defined(ReferenceKind::Weapon, "GUN_A", weapons);
	TEST_EXPECT(gun && project.graph().users_of(*gun).empty());
	if (!gun) return 1;
	editor_test::handle_to_end(project.session, project.request(EditorRequestKind::RenameSymbol, *gun, "GUN_B"));
	TEST_EXPECT(project.session.outcome().unsaved_prompt &&
	            project.view().dialogs.unsaved_prompt.files == std::vector<std::string>{items});
	EditorRequest save = request::resolve_unsaved(UnsavedChoice::Save);
	TEST_EXPECT(editor_test::handle_to_end(project.session, save).done());
	TEST_EXPECT(project.read(items).find("GUN_A") == std::string::npos && project.read(items).find("GUN_C") != std::string::npos);
	TEST_EXPECT(project.defined(ReferenceKind::Weapon, "GUN_B", weapons) && project.graph().missing().empty());
	return 0;
}

// A style variable a menu names as its fonts (the blank project's): every font field is a use
// the rename rewrites, as %NAME%, each resolving after (D9 review).
static int test_style_variable_as_font() {
	Project project("opennova_rename_font_variable");
	const std::string sheet = project.path("menu_style.mns"), menu = project.path("main.mnu");
	const GraphSymbol *font = project.defined(ReferenceKind::StyleVar, "DEF_FONTNAME_LG", sheet);
	TEST_EXPECT(font && !font->inert);
	if (!font) return 1;
	const DialogsView::RenamePreview &plan = project.preview(*font, "DEF_FONTNAME_BIG");
	TEST_EXPECT(plan.refusals.empty() && sites_in(plan, menu) >= 1);
	for (const RenameSite &site : *plan.sites)
		if (site.file == menu) TEST_EXPECT(site.before == "%DEF_FONTNAME_LG%" && site.after == "%DEF_FONTNAME_BIG%");
	TEST_EXPECT(project.rename(*font, "DEF_FONTNAME_BIG"));
	TEST_EXPECT(project.graph().style_binding("DEF_FONTNAME_BIG") && !project.graph().style_binding("DEF_FONTNAME_LG"));
	TEST_EXPECT(project.read(menu).find("%DEF_FONTNAME_LG%") == std::string::npos &&
	            project.read(menu).find("%DEF_FONTNAME_BIG%") != std::string::npos);
	TEST_EXPECT(project.graph().missing().empty());
	return 0;
}

// A name the defining document refuses (a stylesheet's name holds no '/'): the preview says so,
// naming the stylesheet, and the rename writes nothing, the menu using it included (D9 review).
static int test_refused_by_a_document() {
	Project project("opennova_rename_refused_name");
	const std::string sheet = project.path("menu_style.mns");
	TEST_EXPECT(project.write(sheet, project.read(sheet) + "TRIM FF102030\r\n"));
	TEST_EXPECT(project.write("menus/e.mnu", screen("E", window("STATIC", "W",
	                                                                 "<APPEARANCE STATE=\"DEFAULT\" TYPE=\"COLOR\">%TRIM%"
	                                                                 "</APPEARANCE>\r\n"))));
	project.rescan();
	const GraphSymbol *trim = project.defined(ReferenceKind::StyleVar, "TRIM", sheet);
	TEST_EXPECT(trim != nullptr);
	if (!trim) return 1;
	TEST_EXPECT(refused(project.preview(*trim, "BAD/NAME"), "rename.site", sheet));
	const std::string menu_before = project.read("menus/e.mnu"), sheet_before = project.read(sheet);
	TEST_EXPECT(!project.rename(*trim, "BAD/NAME"));
	TEST_EXPECT(project.read("menus/e.mnu") == menu_before && project.read(sheet) == sheet_before);
	return 0;
}

// An item id is a number: "0100301" is 100301, another item's, refused; "0100302" is written
// 100302 (D9 review).
static int test_item_id_canonical() {
	Project project("opennova_rename_item_number");
	const std::string items = project.path("items.def");
	TEST_EXPECT(project.write(items, "begin \"One\"\nid 100300\ntype building\nend\n"
	                                 "begin \"Two\"\nid 100301\ntype building\nend\n"));
	project.rescan();
	const GraphSymbol *item = project.defined(ReferenceKind::Item, "100300", items);
	TEST_EXPECT(item != nullptr);
	if (!item) return 1;
	TEST_EXPECT(refused(project.preview(*item, "0100301"), "rename.exists", items));
	TEST_EXPECT(project.view().dialogs.rename_preview.requested == "0100301");
	const DialogsView::RenamePreview &plan = project.preview(*item, "0100302");
	// The preview is the typed name's (the dialog compares what it asked), its new name the number.
	TEST_EXPECT(plan.refusals.empty() && plan.requested == "0100302" && plan.new_name == "100302" &&
	            plan.sites->size() == 1 && (*plan.sites)[0].after == "100302");
	TEST_EXPECT(project.rename(*item, "0100302"));
	TEST_EXPECT(project.defined(ReferenceKind::Item, "100302", items) && project.read(items).find("100302") != std::string::npos);
	return 0;
}

// An item id's new name sent over the wire as a number (the editor MCP's `new_name` 100302, which
// the edit value that carried it before S13 A4 took): read as its digits, and renamed as the text.
static int test_item_id_sent_as_a_number() {
	Project project("opennova_rename_item_json_number");
	const std::string items = project.path("items.def");
	TEST_EXPECT(project.write(items, "begin \"One\"\nid 100300\ntype building\nend\n"));
	project.rescan();
	const GraphSymbol *item = project.defined(ReferenceKind::Item, "100300", items);
	TEST_EXPECT(item != nullptr);
	if (!item)
		return 1;
	opennova::io::JsonValue json = opennova::io::JsonValue::make_object();
	json.set("kind", opennova::io::json_string("rename_symbol"));
	json.set("path", opennova::io::json_string(item->file));
	json.set("locator", opennova::io::json_string(item->locator));
	json.set("field", opennova::io::json_string(item->field));
	json.set("new_name", opennova::io::json_number(100302));
	EditorRequest rename;
	std::string error;
	TEST_EXPECT(editor_request_from_json(json, rename, error) && rename.new_name == "100302");
	TEST_EXPECT(editor_test::handle_to_end(project.session, rename).done());
	TEST_EXPECT(project.defined(ReferenceKind::Item, "100302", items) &&
			project.read(items).find("100302") != std::string::npos &&
			project.read(items).find("100300") == std::string::npos);
	return 0;
}

// The files written together (review 2): items.def replaced, then weapon.def refused (a
// write-protected file; here the replace step fails it). items.def gets its bytes back, weapon.def
// keeps its own, no written text is left beside either, and the findings say nothing was renamed.
// The same rename then goes through.
static int test_written_together() {
	Project project("opennova_rename_together");
	const std::string weapons = project.path("weapon.def"), items = project.path("items.def");
	TEST_EXPECT(project.write(weapons, "weapon \"GUN_A\"\nend\n"));
	TEST_EXPECT(project.write(items, "begin \"Carrier\"\nid 100300\ntype vehicle\nprimary_weapon GUN_A\nend\n"));
	project.rescan();
	const GraphSymbol *gun = project.defined(ReferenceKind::Weapon, "GUN_A", weapons);
	TEST_EXPECT(gun != nullptr);
	if (!gun) return 1;
	const SymbolRenamePlan plan = plan_symbol_rename_project(*project.view().project.scan, project.graph(), *gun, "GUN_B");
	TEST_EXPECT(plan.ok() && plan.sites.size() == 2);
	const std::string weapons_before = project.read(weapons), items_before = project.read(items);
	const ProjectPaths paths = ProjectPaths::for_root(project.root());
	std::vector<std::string> replaced;
	const FileReplace refuse_weapons = [&](const std::string &from, const std::string &to, std::string &error) {
		replaced.push_back(fs::path(to).filename().string());
		if (replaced.back() == "weapon.def") {
			error = "cannot replace " + to + ": write-protected";
			return false;
		}
		return replace_file(from, to, error);
	};
	std::vector<Diagnostic> findings;
	TEST_EXPECT(!apply_symbol_rename(paths, *project.view().project.document, *project.view().project.scan, project.graph(), plan, findings,
	                                 refuse_weapons));
	TEST_EXPECT((replaced == std::vector<std::string>{"items.def", "weapon.def"}));
	TEST_EXPECT(project.read(items) == items_before && project.read(weapons) == weapons_before);
	TEST_EXPECT(!fs::exists(project.root() + "/" + items + ".tmp") && !fs::exists(project.root() + "/" + weapons + ".tmp"));
	TEST_EXPECT(!findings.empty() && findings[0].code() == "rename.write" &&
	            findings[0].message.find("write-protected") != std::string::npos);
	findings.clear();
	TEST_EXPECT(apply_symbol_rename(paths, *project.view().project.document, *project.view().project.scan, project.graph(), plan, findings));
	TEST_EXPECT(project.read(items).find("GUN_B") != std::string::npos && project.read(weapons).find("GUN_B") != std::string::npos);
	return 0;
}

// A name its definition would hold in another form (review 2): a stylesheet trims a variable's
// name, so " BIG " would define BIG while the menu's fonts were written % BIG %. Refused, naming
// the stylesheet, and nothing is written; also where BIG is defined already.
static int test_name_as_its_definition_holds_it() {
	Project project("opennova_rename_trimmed");
	const std::string sheet = project.path("menu_style.mns"), menu = project.path("main.mnu");
	TEST_EXPECT(project.write(sheet, project.read(sheet) + "BIG FF102030\r\n"));
	project.rescan();
	const GraphSymbol *font = project.defined(ReferenceKind::StyleVar, "DEF_FONTNAME_LG", sheet);
	TEST_EXPECT(font != nullptr);
	if (!font) return 1;
	TEST_EXPECT(refused(project.preview(*font, " LARGE "), "rename.name", sheet));
	TEST_EXPECT(refused(project.preview(*font, " BIG "), "rename.name", sheet));
	const std::string sheet_before = project.read(sheet), menu_before = project.read(menu);
	TEST_EXPECT(!project.rename(*font, " LARGE "));
	TEST_EXPECT(project.read(sheet) == sheet_before && project.read(menu) == menu_before);
	TEST_EXPECT(project.preview(*font, "LARGE").refusals.empty());
	return 0;
}

// An open menu that would not write as it stands (review 2: a quote in a window's name): a rename
// with a use in it is refused, naming it, not planned over the older file on disk. A rename of the
// stylesheet's shadowed SHADOWED, whose name the menu's saved %SHADOWED% does not reach (brand.mns's
// does), goes through without asking to save the menu.
static int test_open_menu_that_does_not_write() {
	Project project("opennova_rename_unwritable");
	const std::string sheet = project.path("menu_style.mns");
	TEST_EXPECT(project.write(sheet, project.read(sheet) + "SHADOWED FF000000\r\n"));
	TEST_EXPECT(project.write("menus/brand.mns", "SHADOWED FF102030\r\n"));
	TEST_EXPECT(project.write("menus/c.mnu", screen("C", window("STATIC", "W",
	                                                                 "<APPEARANCE STATE=\"DEFAULT\" TYPE=\"COLOR\">%SHADOWED%"
	                                                                 "</APPEARANCE>\r\n"))));
	project.rescan();
	editor_test::handle_to_end(project.session, request::open_document("menus/c.mnu"));
	Document *menu = project.session.document_for("menus/c.mnu");
	NodeAddress w;
	TEST_EXPECT(menu && find_definition(AssetGraph(), *menu, "W", w));
	if (!menu) return 1;
	EditorRequest edit = request::edit_record("menus/c.mnu", Edit());
	edit.edits[0].address = w;
	edit.edits[0].field = "name";
	edit.edits[0].value = std::string("W\"Q");
	editor_test::handle_to_end(project.session, edit);
	TEST_EXPECT(menu->dirty() && !menu->serialize().ok());
	const GraphSymbol *brand = project.defined(ReferenceKind::StyleVar, "SHADOWED", "menus/brand.mns");
	const GraphSymbol *plain = project.defined(ReferenceKind::StyleVar, "SHADOWED", sheet);
	TEST_EXPECT(brand && plain && plain->inert);
	if (!brand || !plain) return 1;
	TEST_EXPECT(refused(project.preview(*brand, "BRANDED"), "rename.site", "menus/c.mnu"));
	const std::string menu_before = project.read("menus/c.mnu");
	TEST_EXPECT(project.rename(*plain, "OLDSHADE"));
	TEST_EXPECT(!project.view().dialogs.unsaved_prompt.open);
	TEST_EXPECT(project.read(sheet).find("OLDSHADE") != std::string::npos && project.read("menus/c.mnu") == menu_before);
	return 0;
}

int main() {
	int failures = 0;
	failures += test_written_together();
	failures += test_name_as_its_definition_holds_it();
	failures += test_open_menu_that_does_not_write();
	failures += test_saved_use_behind_an_edit();
	failures += test_style_variable_as_font();
	failures += test_refused_by_a_document();
	failures += test_item_id_canonical();
	failures += test_item_id_sent_as_a_number();
	failures += test_menu_screen();
	failures += test_style_variable();
	failures += test_style_variable_as_text();
	failures += test_style_variable_in_any_text();
	failures += test_string_key();
	failures += test_weapon_name();
	failures += test_ammo_name();
	failures += test_powerup_name();
	failures += test_item_id_refused();
	failures += test_user_point();
	if (failures == 0) std::printf("editor_symbol_rename: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
