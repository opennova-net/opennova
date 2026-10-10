// Missing names offer "Add it there" (ADR 0046 DI-15): a reference to a name no file defines offers, first
// among its fixes, a record of the name's kind added to the file where the game looks the name up, named as
// referenced, born as the file type's own Add makes one (DocumentType::define_symbol), an edit_record of that
// file opened first that selects what it made, one step its Undo takes back; offered only where the batch
// applies and the file, written as its Save would, defines the name where the lookup finds it. Covered over a
// real session from closed files: a weapon, an ammo, a powerup row; an item on the id a mission names (any id
// a marker on it, an id the engine keeps for a place the engine's own row on it, by the reserved-id rule); a
// string id in its section (a new section, then the one now there); a menu's screen and window; a style
// variable in menu_style.mns; a sound set in a bank the game searches (not a bank it never searches) and in a
// menu SOUND's own bank; a sound profile; an effect block in a particle file, revealed where it went. None for
// a set name the bank cannot hold, whose Open fix stays. And DI-33's: a name whose file the project lacks offers
// Create that file with it (a sound bank, a menu, a particle file, a string table).
#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/documents/def_table.h>
#include <editor/documents/document_types.h>
#include <editor/documents/mission_document.h>
#include <editor/session/preferences_store.h>
#include <editor/session/problem_fixes.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/def/def.h>
#include <formats/def/reserved_items.h>
#include <formats/rtxt/rtxt.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;

namespace {

using editor_test::NoProcess;

std::string fixture(const char *relative) { return std::string(test_paths_repo_root(__FILE__)) + "/fixtures/" + relative; }

Edit set_of(const NodeAddress &address, const char *field, Value value) {
	Edit edit;
	edit.address = address;
	edit.field = field;
	edit.value = std::move(value);
	return edit;
}

// A row added with its name, then its fields set, as one batch.
std::vector<Edit> row_with(NodeKind kind, const char *name_field, const std::string &name,
                           const std::vector<std::pair<const char *, std::string>> &fields) {
	std::vector<Edit> edits;
	Edit add;
	add.operation = EditOperation::Add;
	add.address.kind = kind;
	add.field = name_field;
	add.value = name;
	edits.push_back(std::move(add));
	for (const auto &[field, value] : fields) edits.push_back(set_of({batch_made(0), kind, 0}, field, value));
	return edits;
}

struct Fixture {
	editor_test::TempProjectDir dir;
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	std::string root;

	explicit Fixture(const char *name) : dir(name) {
		editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Add it there"));
		editor_test::set_missions(session, true); // ammo.def is a mission's
		editor_test::create_missing_files(session);
		root = session.view().project.root;
	}
	const SessionView &view() const { return session.view(); }
	// A project file's path ("" for none).
	std::string path_of(const char *name) const {
		const AssetEntry *entry = view().project.scan->find(name);
		return entry ? entry->relative_path : std::string();
	}
	// The project's file `name`, loaded through its type, `edits` applied, and written again by the type's writer.
	bool edit_file(const char *name, const std::vector<Edit> &edits) {
		const std::string path = path_of(name);
		const AssetEntry *entry = view().project.scan->find(name);
		const DocumentType *type = entry ? document_type_for(entry->kind) : nullptr;
		std::unique_ptr<Document> document = type ? records_of(type->make()) : nullptr;
		Diagnostic error;
		if (!document || !document->load(root + "/" + path, path, entry->kind, "jo", error) || !document->apply(edits, error)) {
			std::fprintf(stderr, "%s: %s\n", name, error.message.c_str());
			return false;
		}
		const std::string text = document->serialize().text;
		return editor_test::write_bytes(root + "/" + path, std::vector<uint8_t>(text.begin(), text.end()));
	}
	// The reference.missing finding naming `target` (of `kind`), null for none.
	const Diagnostic *missing(ReferenceKind kind, const std::string &target) const {
		for (const Diagnostic &d : view().findings.diagnostics)
			if (const ReferenceSubject *subject = reference_subject(d);
			    subject && d.code() == "reference.missing" && subject->kind == kind && subject->target == target)
				return &d;
		return nullptr;
	}
	// The fix applied: done, its document opened, unsaved and active, the name's finding gone.
	bool apply(const ProblemFix &fix, ReferenceKind kind, const std::string &target) {
		editor_test::handle_to_end(session, fix.request);
		const DocumentBase *document = session.document_base_for(fix.request.path);
		return session.outcome().done() && document && document->dirty() && view().documents.active == fix.request.path &&
		       !missing(kind, target);
	}
	// What the active document has selected, by its name ("" for none).
	std::string selected_name() {
		const Document *document = session.document_for(view().documents.active);
		return document && view().documents.selection.primary.row ? document->record_name(view().documents.selection.primary)
		                                                         : std::string();
	}
	// Undo of the document at `path`: back as the file has it, the name missing again.
	bool undo(const std::string &path, ReferenceKind kind, const std::string &target) {
		editor_test::handle_to_end(session, request::undo(path));
		const DocumentBase *document = session.document_base_for(path);
		return document && !document->dirty() && missing(kind, target);
	}
};

// The first fix of the finding naming `target`: an Add it there of `file`, its label as given; null when the
// finding has none.
const ProblemFix *add_there(const Fixture &f, std::vector<ProblemFix> &fixes, ReferenceKind kind, const std::string &target,
                            const std::string &file, const std::string &label) {
	const Diagnostic *d = f.missing(kind, target);
	if (!d) {
		std::fprintf(stderr, "no reference.missing finding names %s\n", target.c_str());
		return nullptr;
	}
	fixes = fixes_for(*d, f.view());
	if (fixes.empty()) return nullptr;
	const ProblemFix &first = fixes.front();
	const bool shaped = first.request.kind == EditorRequestKind::EditRecord && first.request.path == file &&
	                    first.request.open_first && !first.bulk && first.detail.find("Undo takes it back") != std::string::npos;
	if (first.label != label || !shaped)
		std::fprintf(stderr, "first fix of %s: '%s' on %s\n", target.c_str(), first.label.c_str(), first.request.path.c_str());
	return first.label == label && shaped ? &first : nullptr;
}

} // namespace

// A weapon, an ammo, a powerup row and a sound profile an item or a weapon names: each a row of its table named
// as referenced, with the values the game's reader gives a new one; selected for editing, one undo step.
static int test_catalog_rows() {
	Fixture f("opennova_editor_add_there_catalog");
	const NodeKind item = node_kind(CatalogKind::Item), weapon = node_kind(CatalogKind::Weapon);
	const std::string powerup_file = f.path_of("powerup.def").empty() ? "powerup.def" : f.path_of("powerup.def");
	TEST_EXPECT(editor_test::write_text(f.root + "/" + powerup_file, editor_test::crlf("powerup PW_OTHER\nend\n")));
	editor_test::handle_to_end(f.session, request::rescan());
	TEST_EXPECT(f.edit_file("items.def", row_with(item, "display_name", "Rifleman",
	                                              {{"primary_weapon", "WPN_GONE"}, {"powerup_def", "PW_DI15"},
	                                               {"sound_profile", "PROF_DI15"}})));
	TEST_EXPECT(f.edit_file("weapon.def", row_with(weapon, "weapon_name", "WPN_DI15", {{"round_type", "AMMO_DI15"}})));
	editor_test::handle_to_end(f.session, request::rescan());
	const std::string weapons = f.path_of("weapon.def"), ammo = f.path_of("ammo.def"), powerups = f.path_of("powerup.def"),
	                  profiles = f.path_of("SndProf.def");
	TEST_EXPECT(!weapons.empty() && !ammo.empty() && !powerups.empty() && !profiles.empty());

	std::vector<ProblemFix> fixes;
	const ProblemFix *fix = add_there(f, fixes, ReferenceKind::Weapon, "WPN_GONE", weapons, "Add WPN_GONE to weapon.def");
	TEST_EXPECT(fix && fixes.size() == 2 && fixes[1].label == "Open weapon.def");
	TEST_EXPECT(f.session.document_base_for(weapons) == nullptr); // the fix opens it
	if (!fix) return 1;
	TEST_EXPECT(f.apply(*fix, ReferenceKind::Weapon, "WPN_GONE") && f.selected_name() == "WPN_GONE");
	// Born as the game's reader opens a weapon: its field of view 80 [orig: AdmDef_InitEntryDefaults @ 0x53ff31].
	Value fov;
	const Document *table = f.session.document_for(weapons);
	TEST_EXPECT(table && table->get(f.view().documents.selection.primary, "renderfov", fov) &&
	            std::holds_alternative<double>(fov) && std::get<double>(fov) == 80.0);
	TEST_EXPECT(f.undo(weapons, ReferenceKind::Weapon, "WPN_GONE"));

	fix = add_there(f, fixes, ReferenceKind::Ammo, "AMMO_DI15", ammo, "Add AMMO_DI15 to ammo.def");
	TEST_EXPECT(fix != nullptr);
	if (!fix) return 1;
	TEST_EXPECT(f.apply(*fix, ReferenceKind::Ammo, "AMMO_DI15") && f.selected_name() == "AMMO_DI15");
	TEST_EXPECT(f.undo(ammo, ReferenceKind::Ammo, "AMMO_DI15"));

	fix = add_there(f, fixes, ReferenceKind::Powerup, "PW_DI15", powerups, "Add PW_DI15 to powerup.def");
	TEST_EXPECT(fix != nullptr);
	if (!fix) return 1;
	TEST_EXPECT(f.apply(*fix, ReferenceKind::Powerup, "PW_DI15") && f.selected_name() == "PW_DI15");
	TEST_EXPECT(f.undo(powerups, ReferenceKind::Powerup, "PW_DI15"));

	fix = add_there(f, fixes, ReferenceKind::SoundProfile, "PROF_DI15", profiles, "Add PROF_DI15 to SndProf.def");
	TEST_EXPECT(fix && fix->detail.find("in place of the first profile") != std::string::npos);
	if (!fix) return 1;
	TEST_EXPECT(f.apply(*fix, ReferenceKind::SoundProfile, "PROF_DI15") && f.selected_name() == "PROF_DI15");
	TEST_EXPECT(f.undo(profiles, ReferenceKind::SoundProfile, "PROF_DI15"));
	return 0;
}

// An item a mission places by an id no item has: an item on that id, a marker as a new item is; on an id the
// engine keeps for a place, the engine's own row there, of its kind, in the editor's words for it.
static int test_item_ids() {
	Fixture f("opennova_editor_add_there_items");

	const AssetKind mission_kind = AssetKind::Mission;
	std::unique_ptr<Document> mission = records_of(document_type_for(mission_kind)->make());
	Diagnostic error;
	TEST_EXPECT(mission && mission->load_bytes(test_io::read_file(fixture("bms/synth_logic.bms")), "di15.bms", mission_kind, "jo", error));
	if (!mission) return 1;
	const MissionDocument &entities = dynamic_cast<const MissionDocument &>(*mission);
	const std::vector<const Node *> items = entities.rows_of(MissionKind::Item), people = entities.rows_of(MissionKind::Organic);
	TEST_EXPECT(!items.empty() && !people.empty());
	if (items.empty() || people.empty()) return 1;
	const int reserved = opennova::def::DEF_ITEM_ID_BASE + 2043; // the map centre, a marker the engine finds by its id
	TEST_EXPECT(opennova::def::reserved_item_by_id(reserved) != nullptr);
	TEST_EXPECT(mission->apply(set_of({items[0]->id, items[0]->kind, 0}, "item", int64_t(100777)), error) &&
	            mission->apply(set_of({people[0]->id, people[0]->kind, 0}, "item", int64_t(reserved)), error));
	const std::string text = mission->serialize().text;
	TEST_EXPECT(editor_test::write_bytes(f.root + "/missions/di15.bms", std::vector<uint8_t>(text.begin(), text.end())));
	editor_test::handle_to_end(f.session, request::rescan());
	const std::string catalog = f.path_of("items.def");

	std::vector<ProblemFix> fixes;
	const ProblemFix *fix = add_there(f, fixes, ReferenceKind::Item, "100777", catalog, "Add item 100777 to items.def");
	TEST_EXPECT(fix != nullptr);
	if (!fix) return 1;
	TEST_EXPECT(f.apply(*fix, ReferenceKind::Item, "100777"));
	const Document *table = f.session.document_for(catalog);
	Value id, type;
	const NodeAddress made = f.view().documents.selection.primary;
	TEST_EXPECT(table && table->get(made, "id", id) && id == Value(int64_t(100777)) && table->get(made, "type", type) &&
	            type == Value(int64_t(opennova::def::DEF_ITEM_TYPE_MARKER)));
	TEST_EXPECT(f.undo(catalog, ReferenceKind::Item, "100777"));

	const std::string named = std::to_string(reserved);
	fix = add_there(f, fixes, ReferenceKind::Item, named, catalog, "Add Map centre (" + named + ") to items.def");
	TEST_EXPECT(fix != nullptr);
	if (!fix) return 1;
	TEST_EXPECT(f.apply(*fix, ReferenceKind::Item, named) && f.selected_name() == "Map centre");
	table = f.session.document_for(catalog);
	TEST_EXPECT(table && table->get(f.view().documents.selection.primary, "id", id) && id == Value(int64_t(reserved)));
	for (const Diagnostic &d : f.view().findings.diagnostics) TEST_EXPECT(d.code() != "catalog.reserved_kind");
	TEST_EXPECT(f.undo(catalog, ReferenceKind::Item, named));
	return 0;
}

// A string id a weapon's loadout name reads in GAMETEXT.BIN's WepDes section: a key there, the section made where
// the table has none; the next id goes into the section now there.
static int test_string_ids() {
	Fixture f("opennova_editor_add_there_strings");
	const NodeKind weapon = node_kind(CatalogKind::Weapon);
	TEST_EXPECT(f.edit_file("weapon.def", row_with(weapon, "weapon_name", "WPN_A", {{"loadout_menu_textid", "WEP_DI15_A"}})));
	TEST_EXPECT(f.edit_file("weapon.def", row_with(weapon, "weapon_name", "WPN_B", {{"loadout_menu_textid", "WEP_DI15_B"}})));
	// A table of one section, Menu: no WepDes yet.
	const std::string table = f.path_of("gametext.bin");
	opennova::rtxt::File strings_file;
	strings_file.sections = {{"Menu", 1}};
	opennova::rtxt::Entry entry;
	entry.key = "MM_EXIT";
	entry.text = "Exit";
	strings_file.entries = {entry};
	std::vector<uint8_t> bytes;
	std::string io_error;
	TEST_EXPECT(!table.empty() && opennova::rtxt::write(strings_file, bytes, io_error) &&
	            editor_test::write_bytes(f.root + "/" + table, bytes));
	editor_test::handle_to_end(f.session, request::rescan());
	std::vector<ProblemFix> fixes;
	const ProblemFix *fix = add_there(f, fixes, ReferenceKind::TextId, "WEP_DI15_A", table, "Add WEP_DI15_A to gametext.bin");
	TEST_EXPECT(fix && fix->request.edits.size() == 2 && fix->detail.find("a new section 'WepDes'") != std::string::npos);
	if (!fix) return 1;
	// Through the wire, as the editor MCP passes a fix back: the problems query's request as it is, the string's
	// kind named as the scan read the closed table (a .bin by its content, which its name alone does not say).
	opennova::io::JsonValue args = opennova::io::JsonValue::make_object();
	args.set("text", opennova::io::json_string("WEP_DI15_A"));
	std::string query_error;
	const opennova::io::JsonValue rows = f.session.query("problems", args, query_error);
	const opennova::io::JsonValue *problems = rows.get("problems");
	TEST_EXPECT(problems && problems->is_array() && problems->array.size() == 1);
	const opennova::io::JsonValue *wire_fixes = problems->array[0].get("fixes");
	TEST_EXPECT(wire_fixes && wire_fixes->is_array() && !wire_fixes->array.empty() &&
	            wire_fixes->array[0].get_string("label", "") == "Add WEP_DI15_A to gametext.bin");
	const opennova::io::JsonValue *wire = wire_fixes->array[0].get("request");
	TEST_EXPECT(wire != nullptr);
	if (!wire) return 1;
	const opennova::io::JsonValue answer = f.session.handle_json(*wire);
	if (!answer.get_bool("ok", false)) std::fprintf(stderr, "wire: %s\n", answer.get_string("error", "").c_str());
	TEST_EXPECT(answer.get_bool("ok", false) && f.session.outcome().done());
	f.session.run_operations();
	const DocumentBase *opened = f.session.document_base_for(table);
	TEST_EXPECT(opened && opened->dirty() && f.view().documents.active == table && !f.missing(ReferenceKind::TextId, "WEP_DI15_A"));
	TEST_EXPECT(f.selected_name() == "WEP_DI15_A");
	editor_test::handle_to_end(f.session, request::save(table));
	fix = add_there(f, fixes, ReferenceKind::TextId, "WEP_DI15_B", table, "Add WEP_DI15_B to gametext.bin");
	TEST_EXPECT(fix && fix->request.edits.size() == 1);
	if (!fix) return 1;
	const Document *strings = f.session.document_for(table);
	const size_t sections = strings ? strings->rows().size() : 0;
	TEST_EXPECT(f.apply(*fix, ReferenceKind::TextId, "WEP_DI15_B") && f.selected_name() == "WEP_DI15_B");
	TEST_EXPECT(strings && strings->rows().size() == sections);
	TEST_EXPECT(f.undo(table, ReferenceKind::TextId, "WEP_DI15_B") && !f.missing(ReferenceKind::TextId, "WEP_DI15_A"));
	return 0;
}

// A menu's ACTIONs naming a screen and a window no lookup finds, its look a style variable no stylesheet defines,
// its SOUND a set its bank lacks: a screen at the menu's end, a window inside the screen's MAIN, a variable in
// menu_style.mns, a set in the SOUND's own bank (not in game.lwf).
static int test_menu_names() {
	Fixture f("opennova_editor_add_there_menu");
	TEST_EXPECT(editor_test::write_bytes(f.root + "/click.lwf", editor_test::sound_bank_of({"MOUSE_OVER"})) &&
	            editor_test::write_bytes(f.root + "/game.lwf", editor_test::sound_bank_of({"FIRE"})));
	TEST_EXPECT(editor_test::write_text(
	        f.root + "/menus/di15.mnu",
	        "<SCREEN>\r\n<NAME>HOME</NAME>\r\n<WINDOW type=\"window\" name=\"MAIN\">\r\n"
	        "<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>\r\n"
	        "<WINDOW type=\"button\" name=\"GO\">\r\n"
	        "<POSITION><LEFT>10</LEFT><TOP>10</TOP><RIGHT>90</RIGHT><BOTTOM>30</BOTTOM></POSITION>\r\n"
	        "<APPEARANCE STATE=\"DEFAULT\" TYPE=\"COLOR\">%DI15_COLOR%</APPEARANCE>\r\n"
	        "<SOUND STATE=\"MOUSEIN\" TRIGGER=\"DI15_CLICK\">click.lwf</SOUND>\r\n"
	        "<ACTION TYPE=\"SCREEN\" FILE=\"di15.mnu\">AWAY</ACTION>\r\n"
	        "<ACTION TYPE=\"WINDOW\" STATE=\"SHOW\">PANEL</ACTION>\r\n"
	        "</WINDOW>\r\n</WINDOW>\r\n</SCREEN>\r\n"));
	editor_test::handle_to_end(f.session, request::rescan());
	const std::string menu = "menus/di15.mnu";

	std::vector<ProblemFix> fixes;
	const ProblemFix *fix = add_there(f, fixes, ReferenceKind::MenuScreen, "AWAY", menu, "Add screen AWAY to di15.mnu");
	TEST_EXPECT(fix != nullptr);
	if (!fix) return 1;
	TEST_EXPECT(f.apply(*fix, ReferenceKind::MenuScreen, "AWAY") && f.selected_name() == "AWAY");
	TEST_EXPECT(f.undo(menu, ReferenceKind::MenuScreen, "AWAY"));

	fix = add_there(f, fixes, ReferenceKind::MenuWindow, "PANEL", menu, "Add window PANEL to di15.mnu");
	TEST_EXPECT(fix && fix->detail.find("inside its window MAIN") != std::string::npos);
	if (!fix) return 1;
	TEST_EXPECT(f.apply(*fix, ReferenceKind::MenuWindow, "PANEL") && f.selected_name() == "PANEL");
	TEST_EXPECT(f.undo(menu, ReferenceKind::MenuWindow, "PANEL"));

	const std::string style = f.path_of("menu_style.mns");
	fix = add_there(f, fixes, ReferenceKind::StyleVar, "%DI15_COLOR%", style, "Add %DI15_COLOR% to menu_style.mns");
	TEST_EXPECT(fix != nullptr);
	if (!fix) return 1;
	TEST_EXPECT(f.apply(*fix, ReferenceKind::StyleVar, "%DI15_COLOR%") && f.selected_name() == "DI15_COLOR");
	TEST_EXPECT(f.undo(style, ReferenceKind::StyleVar, "%DI15_COLOR%"));

	const std::string bank = f.path_of("click.lwf");
	fix = add_there(f, fixes, ReferenceKind::Sound, "DI15_CLICK", bank, "Add DI15_CLICK to click.lwf");
	TEST_EXPECT(fix != nullptr);
	if (!fix) return 1;
	TEST_EXPECT(f.apply(*fix, ReferenceKind::Sound, "DI15_CLICK") && f.selected_name() == "DI15_CLICK");
	TEST_EXPECT(f.undo(bank, ReferenceKind::Sound, "DI15_CLICK"));
	return 0;
}

// A sound set an item names, in the bank the game searches a set by name in (game.lwf), not in a bank it never
// searches; none for a name no set holds (23 characters at most), whose Open fix stays. An effect an item names:
// the effect writer's block after the particle file's last effect, the file shown at it.
static int test_sound_and_effect() {
	Fixture f("opennova_editor_add_there_sound");
	TEST_EXPECT(editor_test::write_bytes(f.root + "/aside.lwf", editor_test::sound_bank_of({"ASIDE"})) &&
	            editor_test::write_bytes(f.root + "/game.lwf", editor_test::sound_bank_of({"FIRE"})));
	TEST_EXPECT(editor_test::write_text(f.root + "/fx.ptl", "[effectdef]\n{\n\tid = FX_OTHER;\n}\n\n[particledef]\n{\n\tid = puff;\n}\n"));
	editor_test::handle_to_end(f.session, request::rescan());
	const std::string too_long(24, 'Q');
	TEST_EXPECT(f.edit_file("items.def", row_with(node_kind(CatalogKind::Item), "display_name", "Door",
	                                              {{"door_open_sound", "DI15_OPEN"}, {"door_close_sound", too_long},
	                                               {"particledeath", "FX_DI15"}})));
	editor_test::handle_to_end(f.session, request::rescan());
	const std::string bank = f.path_of("game.lwf");

	std::vector<ProblemFix> fixes;
	const ProblemFix *fix = add_there(f, fixes, ReferenceKind::Sound, "DI15_OPEN", bank, "Add DI15_OPEN to game.lwf");
	TEST_EXPECT(fix != nullptr);
	if (!fix) return 1;
	TEST_EXPECT(f.apply(*fix, ReferenceKind::Sound, "DI15_OPEN") && f.selected_name() == "DI15_OPEN");
	TEST_EXPECT(f.undo(bank, ReferenceKind::Sound, "DI15_OPEN"));
	TEST_EXPECT(add_there(f, fixes, ReferenceKind::Sound, too_long, bank, "Add " + too_long + " to game.lwf") == nullptr);
	TEST_EXPECT(fixes.size() == 1 && fixes[0].label == "Open game.lwf");

	const std::string particles = f.path_of("fx.ptl");
	fix = add_there(f, fixes, ReferenceKind::Particle, "FX_DI15", particles, "Add effect FX_DI15 to fx.ptl");
	TEST_EXPECT(fix != nullptr);
	if (!fix) return 1;
	const uint64_t seq = f.view().events.held().empty() ? 0 : f.view().events.held().back().seq;
	TEST_EXPECT(f.apply(*fix, ReferenceKind::Particle, "FX_DI15"));
	const DocumentBase *document = f.session.document_base_for(particles);
	const TextDocument *text = document ? text_of(*document) : nullptr;
	TEST_EXPECT(text && text->text() ==
	                            "[effectdef]\n{\n\tid = FX_OTHER;\n}\n\n[effectdef]\n{\n\tid = FX_DI15;\n\n}\n\n[particledef]\n{\n\tid = puff;\n}\n");
	const std::vector<ViewEvent> shown = editor_test::events_after(f.view(), seq, ViewEventKind::RevealText);
	TEST_EXPECT(shown.size() == 1 && shown[0].path == particles && shown[0].locator == "5:1");
	TEST_EXPECT(f.undo(particles, ReferenceKind::Particle, "FX_DI15"));
	return 0;
}

// The fix of a missing name whose file the project lacks (ADR 0046 DI-33): Create that file with it, the file made by
// the engine's writer (its requirement's blank, else its kind's) and the name added to it as its type's Add makes one,
// one create_file with its define; the file then made, opened and active, the name selected there, the finding gone,
// Undo of the file taking the name back. A sound set with no bank (the first bank the game searches), an effect with
// no particle file (a new particle file: the game reads every one), a string id whose table the project lacks
// (through the wire as the problems query writes it); and a menu an ACTION loads, which the menu's own Create makes.
static int test_create_with_it() {
	Fixture f("opennova_editor_add_there_create");
	TEST_EXPECT(editor_test::write_text(
	        f.root + "/menus/di33.mnu",
	        "<SCREEN>\r\n<NAME>HOME</NAME>\r\n<WINDOW type=\"window\" name=\"MAIN\">\r\n"
	        "<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>\r\n"
	        "<WINDOW type=\"button\" name=\"GO\">\r\n"
	        "<POSITION><LEFT>10</LEFT><TOP>10</TOP><RIGHT>90</RIGHT><BOTTOM>30</BOTTOM></POSITION>\r\n"
	        "<ACTION TYPE=\"SCREEN\" FILE=\"away.mnu\">AWAY</ACTION>\r\n"
	        "</WINDOW>\r\n</WINDOW>\r\n</SCREEN>\r\n"));
	TEST_EXPECT(f.edit_file("items.def", row_with(node_kind(CatalogKind::Item), "display_name", "Door",
	                                              {{"door_open_sound", "DI33_OPEN"}, {"particledeath", "FX_DI33"}})));
	editor_test::handle_to_end(f.session, request::rescan());
	TEST_EXPECT(f.path_of("gamelocl.lwf").empty() && f.path_of("away.mnu").empty());

	// The case shared by each: the finding's first fix creates `file` with the name, and applied makes it, opens it
	// and selects the name there; Undo of the file takes the name back, the file staying.
	const auto created = [&](ReferenceKind kind, const std::string &target, const std::string &file) {
		const Diagnostic *d = f.missing(kind, target);
		const std::vector<ProblemFix> fixes = d ? fixes_for(*d, f.view()) : std::vector<ProblemFix>();
		if (fixes.empty()) {
			std::fprintf(stderr, "no fix for %s\n", target.c_str());
			return false;
		}
		const ProblemFix &fix = fixes.front();
		const bool shaped = fix.label == "Create " + file + " with " + target && !fix.bulk &&
		                    fix.request.kind == EditorRequestKind::CreateFile && fix.request.path == file &&
		                    fix.request.define.kind == kind && fix.request.define.target == target &&
		                    fix.detail.find("It cannot be undone with Undo.") != std::string::npos && has_fixes(*d, f.view());
		if (!shaped) {
			std::fprintf(stderr, "first fix of %s: '%s'\n", target.c_str(), fix.label.c_str());
			return false;
		}
		editor_test::handle_to_end(f.session, fix.request);
		const std::string path = f.path_of(file.c_str());
		const DocumentBase *document = path.empty() ? nullptr : f.session.document_base_for(path);
		const bool made = f.session.outcome().done() && document && document->dirty() && f.view().documents.active == path &&
		                  !f.missing(kind, target);
		if (!made) std::fprintf(stderr, "%s not made with %s\n", file.c_str(), target.c_str());
		return made && f.undo(path, kind, target) && !f.path_of(file.c_str()).empty();
	};
	TEST_EXPECT(created(ReferenceKind::Sound, "DI33_OPEN", "gamelocl.lwf"));
	// A screen an ACTION looks up in a menu the project lacks: the menu file's own reference is the one missing (the
	// screen's lookup unverified until the file is there), and its Create makes the menu, its one screen named after
	// the file (AWAY), which the lookup then finds.
	const Diagnostic *menu = f.missing(ReferenceKind::Menu, "away.mnu");
	const std::vector<ProblemFix> menu_fixes = menu ? fixes_for(*menu, f.view()) : std::vector<ProblemFix>();
	TEST_EXPECT(!f.missing(ReferenceKind::MenuScreen, "AWAY") && !menu_fixes.empty() && menu_fixes.back().label == "Create away.mnu");
	if (menu_fixes.empty()) return 1;
	editor_test::handle_to_end(f.session, menu_fixes.back().request);
	TEST_EXPECT(!f.path_of("away.mnu").empty() && !f.missing(ReferenceKind::Menu, "away.mnu") &&
	            !f.missing(ReferenceKind::MenuScreen, "AWAY"));

	// The effect: a new particle file, the effect writer's block in it, revealed where it went.
	const Diagnostic *effect = f.missing(ReferenceKind::Particle, "FX_DI33");
	const std::vector<ProblemFix> effect_fixes = effect ? fixes_for(*effect, f.view()) : std::vector<ProblemFix>();
	TEST_EXPECT(!effect_fixes.empty() && effect_fixes.front().request.kind == EditorRequestKind::CreateFile &&
	            effect_fixes.front().label == "Create " + effect_fixes.front().request.path + " with FX_DI33");
	if (effect_fixes.empty()) return 1;
	editor_test::handle_to_end(f.session, effect_fixes.front().request);
	const std::string particles = f.path_of(effect_fixes.front().request.path.c_str());
	const DocumentBase *document = particles.empty() ? nullptr : f.session.document_base_for(particles);
	const TextDocument *text = document ? text_of(*document) : nullptr;
	TEST_EXPECT(text && text->text().find("id = FX_DI33;") != std::string::npos && !f.missing(ReferenceKind::Particle, "FX_DI33"));

	// A string id a weapon's loadout name reads in GAMETEXT.BIN, which the project lacks: through the wire, as the
	// editor MCP passes a fix back.
	const NodeKind weapon = node_kind(CatalogKind::Weapon);
	TEST_EXPECT(f.edit_file("weapon.def", row_with(weapon, "weapon_name", "WPN_DI33", {{"loadout_menu_textid", "WEP_DI33"}})));
	const std::string gametext = f.path_of("gametext.bin");
	TEST_EXPECT(!gametext.empty() && std::remove((f.root + "/" + gametext).c_str()) == 0);
	editor_test::handle_to_end(f.session, request::rescan());
	opennova::io::JsonValue args = opennova::io::JsonValue::make_object();
	args.set("text", opennova::io::json_string("WEP_DI33"));
	std::string query_error;
	const opennova::io::JsonValue rows = f.session.query("problems", args, query_error);
	const opennova::io::JsonValue *problems = rows.get("problems");
	TEST_EXPECT(problems && problems->is_array() && problems->array.size() == 1);
	if (!problems || problems->array.size() != 1) return 1;
	const opennova::io::JsonValue *wire_fixes = problems->array[0].get("fixes");
	TEST_EXPECT(wire_fixes && wire_fixes->is_array() && !wire_fixes->array.empty());
	if (!wire_fixes || wire_fixes->array.empty()) return 1;
	const opennova::io::JsonValue *wire = wire_fixes->array[0].get("request");
	TEST_EXPECT(wire_fixes->array[0].get_string("label", "").rfind("Create gametext.bin with WEP_DI33", 0) == 0 && wire &&
	            wire->get("define") && wire->get("define")->get_string("kind", "") == "text_id");
	if (!wire) return 1;
	const opennova::io::JsonValue answer = f.session.handle_json(*wire);
	if (!answer.get_bool("ok", false)) std::fprintf(stderr, "wire: %s\n", answer.get_string("error", "").c_str());
	TEST_EXPECT(answer.get_bool("ok", false) && f.session.outcome().done());
	f.session.run_operations();
	TEST_EXPECT(!f.path_of("gametext.bin").empty() && !f.missing(ReferenceKind::TextId, "WEP_DI33") &&
	            f.selected_name() == "WEP_DI33");
	return 0;
}

int main() {
	if (test_catalog_rows() != 0) return 1;
	if (test_item_ids() != 0) return 1;
	if (test_string_ids() != 0) return 1;
	if (test_menu_names() != 0) return 1;
	if (test_sound_and_effect() != 0) return 1;
	if (test_create_with_it() != 0) return 1;
	return 0;
}
