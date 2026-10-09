#include <editor/documents/def_catalog_document.h>
#include <editor/documents/catalog_validation.h>
#include <editor/documents/def_words.h>
#include <editor/graph/display_names.h>
#include <base/io/strutil.h>
#include <editor/graph/asset_graph.h>
#include <editor/model/field_text.h>
#include <editor/project_build/build_run.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/project/project_files.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"
#include "common/file_io.h"
#include "common/test_expect.h"
#include <algorithm>
#include <cstring>

using namespace opennova::editor;
using namespace opennova::def;

using editor_test::NoProcess;

static const CatalogRow &row_at(const Document &document, size_t index) {
	return static_cast<const CatalogRow &>(*document.rows()[index]);
}
static Edit field(NodeAddress address, std::string key, DefValue value, bool coalesce = false) {
	Edit edit; edit.address = address; edit.field = std::move(key);
	edit.value = std::move(value); edit.coalesce = coalesce; return edit;
}
static int history_and_save() {
	editor_test::TempProjectDir dir("opennova_catalog_document_test");
	TEST_EXPECT(editor_test::write_text(dir.file("items.def"), "begin \"One\"\nid 100001\ntype marker\nhp 10\nend\n"));
	DefCatalogDocument document; Diagnostic error;
	TEST_EXPECT(document.load(dir.file("items.def"), "items.def", AssetKind::ItemDefs, "jo", error));
	const auto id = document.rows()[0]->id;
	const NodeAddress row{id, node_kind(DefRecordKind::Item), 0};
	TEST_EXPECT(document.apply(field(row, "hp", int64_t(20), true), error));
	TEST_EXPECT(document.apply(field(row, "hp", int64_t(25), true), error));
	TEST_EXPECT(document.dirty());
	document.undo();
	TEST_EXPECT(!document.dirty());
	TEST_EXPECT(row_at(document, 0).native.as<DefItemDef>().hp == 10);
	document.redo();
	TEST_EXPECT(row_at(document, 0).native.as<DefItemDef>().hp == 25);
	TEST_EXPECT(document.save(error));
	TEST_EXPECT(!document.dirty());
	document.undo(); TEST_EXPECT(document.dirty());
	document.redo(); TEST_EXPECT(!document.dirty());
	TEST_EXPECT(document.apply(field(row, "graphic", std::string("missing_model")), error));
	TEST_EXPECT(document.save(error)); // a serializable draft with a missing dependency

	Edit duplicate; duplicate.address = row; duplicate.operation = EditOperation::Duplicate;
	TEST_EXPECT(document.apply(duplicate, error)); const auto duplicate_id = document.last_added();
	TEST_EXPECT(duplicate_id != id && document.rows().size() == 2);
	duplicate.operation = EditOperation::Move; duplicate.address.row = duplicate_id; duplicate.position = 0;
	TEST_EXPECT(document.apply(duplicate, error));
	TEST_EXPECT(document.rows()[0]->id == duplicate_id);
	document.undo(); TEST_EXPECT(document.rows()[0]->id == id);
	document.undo(); TEST_EXPECT(document.rows().size() == 1);
	document.redo(); TEST_EXPECT(document.rows()[1]->id == duplicate_id);
	document.undo();
	TEST_EXPECT(document.apply(field(row, "hp", int64_t(30)), error));
	TEST_EXPECT(!document.can_redo()); // a new branch cannot redo the removed duplicate

	// A same-size external rewrite, even with the old timestamp, must conflict.
	const auto stamp = std::filesystem::last_write_time(dir.file("items.def"));
	std::string external, message;
	TEST_EXPECT(opennova::io::read_file_text(dir.file("items.def"), external, message));
	const auto hp = external.find("hp 25"); TEST_EXPECT(hp != std::string::npos);
	external.replace(hp, 5, "hp 99");
	TEST_EXPECT(editor_test::write_text(dir.file("items.def"), external));
	std::filesystem::last_write_time(dir.file("items.def"), stamp);
	TEST_EXPECT(!document.save(error) && error.code() == "document.conflict");
	std::string retained; TEST_EXPECT(opennova::io::read_file_text(dir.file("items.def"), retained, message));
	TEST_EXPECT(retained == external && document.dirty());

	DefCatalogDocument reloaded;
	TEST_EXPECT(reloaded.load(dir.file("items.def"), "items.def", AssetKind::ItemDefs, "jo", error));
	TEST_EXPECT(row_at(reloaded, 0).native.as<DefItemDef>().hp == 99);
	TEST_EXPECT(reloaded.apply(field(row, "hp", int64_t(7)), error));
	std::filesystem::create_directory(dir.file("items.def.tmp"));
	TEST_EXPECT(!reloaded.save(error) && error.code() == "document.write" && reloaded.dirty());
	TEST_EXPECT(opennova::io::read_file_text(dir.file("items.def"), retained, message) && retained == external);
	return 0;
}
// Two items added in one batch (S13 D7): each new item takes an id no row of the batch has, the
// second skipping the first's (make_node reads the rows as the batch left them), one undo step.
static int two_new_items() {
	editor_test::TempProjectDir dir("opennova_catalog_two_new_items");
	TEST_EXPECT(editor_test::write_text(dir.file("items.def"), "begin \"One\"\nid 100001\ntype marker\nhp 10\nend\n"));
	DefCatalogDocument document; Diagnostic error;
	TEST_EXPECT(document.load(dir.file("items.def"), "items.def", AssetKind::ItemDefs, "jo", error));
	Edit add; add.operation = EditOperation::Add; add.address = {0, node_kind(DefRecordKind::Item), 0};
	TEST_EXPECT(document.apply(std::vector<Edit>{add, add}, error) && document.rows().size() == 3);
	TEST_EXPECT(row_at(document, 1).native.as<DefItemDef>().id == 100000 &&
	            row_at(document, 2).native.as<DefItemDef>().id == 100002);
	document.undo(); TEST_EXPECT(document.rows().size() == 1 && !document.dirty());
	return 0;
}
static int collections() {
	editor_test::TempProjectDir dir("opennova_catalog_collections_test");
	TEST_EXPECT(editor_test::write_text(dir.file("weapon.def"), "weapon \"WPN_ONE\"\nend\n"));
	DefCatalogDocument document; Diagnostic error;
	TEST_EXPECT(document.load(dir.file("weapon.def"), "weapon.def", AssetKind::WeaponDefs, "jo", error));
	const auto parent = document.rows()[0]->id;
	Edit edit; edit.operation = EditOperation::Add; edit.address = {parent, node_kind(DefRecordKind::Action), 0};
	TEST_EXPECT(document.apply(edit, error)); const auto child = document.last_added();
	const NodeAddress action{parent, node_kind(DefRecordKind::Action), child};
	// A weapon holds its actions and sights (owner-scoped, S9g); an action holds nothing.
	const NodeAddress weapon_row{parent, node_kind(DefRecordKind::Weapon), 0};
	const std::vector<Document::Collection> held = document.collections_of(weapon_row);
	TEST_EXPECT(held.size() == 2 && std::string(document.kind_token(held[0].spec.kind)) == "action" && held[0].ids == std::vector<NodeId>{child} &&
	            std::string(document.kind_token(held[1].spec.kind)) == "sight" && held[1].ids.empty());
	Document::Placement at;
	TEST_EXPECT(document.collections_of(action).empty() && document.placement(action, at) && at.owner == weapon_row);
	// B5: the only action moved to its own place is no edit.
	const uint64_t revision = document.revision();
	Edit stay; stay.operation = EditOperation::Move; stay.address = action; stay.position = 0;
	TEST_EXPECT(document.apply(stay, error) && document.revision() == revision);
	TEST_EXPECT(document.apply(field(action, "name", std::string("Fire")), error));
	TEST_EXPECT(document.apply(field(action, "function", std::string("Shoot")), error));
	TEST_EXPECT(document.apply(field(action, "function_args_count", int64_t(2)), error));
	TEST_EXPECT(document.apply(field(action, "function_args[0]", int64_t(8)), error));
	TEST_EXPECT(document.apply(field(action, "ctrl_register", std::string("trigger")), error));
	TEST_EXPECT(document.apply(field(action, "ctrl_increment", int64_t(1)), error));
	TEST_EXPECT(document.apply(field(action, "duplicate_sound_delay", int64_t(3)), error));
	TEST_EXPECT(document.apply(field({parent, node_kind(DefRecordKind::Weapon), 0}, "weaponweight", 1.25), error));
	TEST_EXPECT(row_at(document, 0).native.as<DefWeaponDef>().weaponweight_fp16 == 81920);
    TEST_EXPECT(!document.apply(field(action, "function_args_count", int64_t(5)), error));
    TEST_EXPECT(document.apply(field(action, "function_args_count", int64_t(1)), error));
    TEST_EXPECT(static_cast<const DefWeaponAction *>(document.record(action))->function_args[1] == 0);
    document.undo();
    TEST_EXPECT(document.save(error));
    edit.operation = EditOperation::Duplicate; edit.address = action;
	TEST_EXPECT(document.apply(edit, error)); const auto duplicate = document.last_added();
	TEST_EXPECT(duplicate != child);
	edit.operation = EditOperation::Remove; edit.address.child = child;
	TEST_EXPECT(document.apply(edit, error)); TEST_EXPECT(document.record(action) == nullptr);
	document.undo(); TEST_EXPECT(document.record(action) != nullptr);
	TEST_EXPECT(static_cast<const DefWeaponAction *>(document.record(action))->ctrl_increment == 1);
	document.undo(); TEST_EXPECT(!document.dirty());
	DefWeaponsFile parsed{}; TEST_EXPECT(def_parse_weapons(dir.file("weapon.def").c_str(), &parsed) == 0);
	TEST_EXPECT(parsed.count == 1 && parsed.entries[0].actions_count == 1);
	TEST_EXPECT(parsed.entries[0].actions[0].function_args_count == 2 && parsed.entries[0].actions[0].function_args[0] == 8);
	def_free_weapons(&parsed);
	return 0;
}
// The review's Y2 through the catalog: a sight duplicated in its weapon is a record of its own (its copy
// names none of the file's layout, prepare_record: written in the writer's form after its original), and
// one moved follows the weapon's order; the comment above each stands with it, and no record is written
// in the writer's form (the save says nothing).
static int nested_rows_follow_the_record() {
	editor_test::TempProjectDir dir("opennova_catalog_nested_rows");
	// The text as the file holds it, CR LF (editor_test::crlf: the one break the def walk splits at).
	const std::string text = editor_test::crlf("weapon \"WPN_S\"\n\t// the scope\n\tsights scope.tga 0 0 640 480 blend\n"
	                                           "\t// the dot\n\tsights reticle.tga 0 0 64 64 add\nend\n");
	TEST_EXPECT(editor_test::write_text(dir.file("weapon.def"), text));
	DefCatalogDocument document; Diagnostic error;
	TEST_EXPECT(document.load(dir.file("weapon.def"), "weapon.def", AssetKind::WeaponDefs, "jo", error));
	const auto parent = document.rows()[0]->id;
	const NodeAddress weapon_row{parent, node_kind(DefRecordKind::Weapon), 0};
	std::vector<NodeId> sights;
	for (const Document::Collection &collection : document.collections_of(weapon_row))
		if (std::string(document.kind_token(collection.spec.kind)) == "sight") sights = collection.ids;
	TEST_EXPECT(sights.size() == 2);
	if (sights.size() != 2) return 1;
	TEST_EXPECT(document.serialize().text == text);
	Edit duplicate; duplicate.operation = EditOperation::Duplicate;
	duplicate.address = {parent, node_kind(DefRecordKind::Sight), sights[0]};
	TEST_EXPECT(document.apply(duplicate, error));
	SerializeResult written = document.serialize();
	TEST_EXPECT(written.ok() && written.notes.empty() &&
	            written.text == editor_test::crlf("weapon \"WPN_S\"\n\t// the scope\n\tsights scope.tga 0 0 640 480 blend\n"
	                                              "\tsights scope.tga 0 0 640 480 blend\n\t// the dot\n\tsights reticle.tga 0 0 64 64 add\nend\n"));
	document.undo();
	Edit move; move.operation = EditOperation::Move;
	move.address = {parent, node_kind(DefRecordKind::Sight), sights[1]};
	move.position = 0;
	TEST_EXPECT(document.apply(move, error));
	written = document.serialize();
	TEST_EXPECT(written.ok() && written.notes.empty() &&
	            written.text == editor_test::crlf("weapon \"WPN_S\"\n\t// the dot\n\tsights reticle.tga 0 0 64 64 add\n"
	                                              "\t// the scope\n\tsights scope.tga 0 0 640 480 blend\nend\n"));
	return 0;
}
static int session_gate() {
	editor_test::TempProjectDir dir("opennova_catalog_session_test");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(request::new_project(dir.file("project"), "Catalog"));
	session.run_operations();
	editor_test::create_missing_files(session);
	// Play asks first, as with the project's save_before_play off (DI-26: on, Play saves without asking).
	ProjectSettingsChange asks;
	asks.save_before_play = false;
	editor_test::apply_settings(session, asks);
    session.handle(request::create_file("ammo.def"));
    TEST_EXPECT(session.document_for("ammo.def"));
    session.handle(request::create_file("ammo.def"));
    TEST_EXPECT(session.document_for("ammo.def")->rows().size() == 1);
	session.handle(request::open_document("items.def"));
	auto *document = session.document_for(); TEST_EXPECT(document);
	const auto id = document->rows()[0]->id;
	auto request = request::edit_record(std::string(),
			field({ id, node_kind(DefRecordKind::Item), 0 }, "graphic", std::string("missing")));
	session.handle(request);
	// Play packs the files on disk: it waits on the prompt, which lists the edited catalog
	// and offers no Discard; its Save writes the catalog, then Play builds.
	session.handle(request::play());
	const DialogsView::UnsavedPrompt &prompt = session.view().dialogs.unsaved_prompt;
	TEST_EXPECT(platform.spawns == 0 && !session.view().activity.operation.running() && prompt.open && prompt.action == EditorRequestKind::Play);
	TEST_EXPECT(prompt.files == std::vector<std::string>{document->path()} && !prompt.can_discard);
	auto answer = request::of(EditorRequestKind::ResolveUnsaved); answer.choice = UnsavedChoice::Save;
	session.handle(answer); session.run_operations();
	TEST_EXPECT(!document->dirty() && !prompt.open); // semantic errors do not prevent saving
	// The graphic is missing: listed, an error still, and the build lands all the same (S14: a
	// missing reference gates no build).
	size_t missing_graphic = 0;
	for (const Diagnostic &d : session.view().findings.diagnostics)
		missing_graphic += d.code() == "reference.missing" && d.severity == DiagnosticSeverity::Error && d.field == "graphic" ? 1 : 0;
	TEST_EXPECT(session.view().activity.has_build && session.view().activity.last_build->ok && missing_graphic == 1);
	request.edits = {field({id, node_kind(DefRecordKind::Item), 0}, "graphic", std::string(""))};
	session.handle(request);
	session.handle(request::close_project());
	TEST_EXPECT(session.project_open() && prompt.open && prompt.can_discard);
	answer.choice = UnsavedChoice::Cancel;
	session.handle(answer); TEST_EXPECT(session.project_open() && !prompt.open);
	session.handle(request::close_project());
	answer.choice = UnsavedChoice::Save;
	session.handle(answer); TEST_EXPECT(!session.project_open());
	session.handle(request::open_project(dir.file("project")));
	session.run_operations();
	session.handle(request::build()); session.run_operations();
	TEST_EXPECT(session.view().activity.last_build->ok);
	session.handle(request::open_document("items.def"));
	document = session.document_for(); TEST_EXPECT(document && !document->dirty());
	return 0;
}
static int malformed() {
	editor_test::TempProjectDir dir("opennova_catalog_malformed_test");
	TEST_EXPECT(editor_test::write_text(dir.file("items.def"), "begin \"Bad\"\nhp twelve\nend\n"));
	DefCatalogDocument document; Diagnostic error;
	TEST_EXPECT(document.load(dir.file("items.def"), "items.def", AssetKind::ItemDefs, "jo", error));
	TEST_EXPECT(document.blocked() && document.issues().front().line == 2);
	TEST_EXPECT(!document.save(error) && error.code() == "document.unserializable");
	TEST_EXPECT(editor_test::write_text(dir.file("items.def"), "begin \"Open\"\nid 100001\n"));
	TEST_EXPECT(document.load(dir.file("items.def"), "items.def", AssetKind::ItemDefs, "jo", error));
	TEST_EXPECT(document.blocked());
	return 0;
}
// Lines the game ignores never block the document: they are reported, the record
// stays editable, and saving keeps them as the file has them (the file's notes,
// def_notes.h), the edit its own line.
static int ignored_input() {
	editor_test::TempProjectDir dir("opennova_catalog_ignored_test");
	const std::string text = "begin \"One\"\nid 100001\ntype marker\nsubtype Ruins\nattrib: good nodie\nend\n";
	TEST_EXPECT(editor_test::write_text(dir.file("items.def"), text));
	DefCatalogDocument document; Diagnostic error;
	TEST_EXPECT(document.load(dir.file("items.def"), "items.def", AssetKind::ItemDefs, "jo", error));
	TEST_EXPECT(!document.blocked() && document.ignored_lines() == 2 && document.issues().size() == 2);
	const NodeAddress row{document.rows()[0]->id, node_kind(DefRecordKind::Item), 0};
	TEST_EXPECT(document.apply(field(row, "hp", int64_t(20)), error));
	TEST_EXPECT(document.save(error));
	// The findings are the written text's from then on: the kept lines are reported still.
	TEST_EXPECT(document.ignored_lines() == 2 && document.issues().size() == 2 && document.can_undo());
	std::string saved, message;
	TEST_EXPECT(opennova::io::read_file_text(dir.file("items.def"), saved, message));
	// (the new line after the record's lines, indented as they are)
	TEST_EXPECT(saved == editor_test::crlf("begin \"One\"\nid 100001\ntype marker\nsubtype Ruins\nattrib: good nodie\nhp 20\nend\n"));
	TEST_EXPECT(document.load(dir.file("items.def"), "items.def", AssetKind::ItemDefs, "jo", error));
	TEST_EXPECT(document.ignored_lines() == 2 && document.issues().size() == 2);
	TEST_EXPECT((row_at(document, 0).native.as<DefItemDef>().attrib & DEF_ITEM_ATTRIB_NODIE) != 0);
	return 0;
}
// A later action block of a name replaces the earlier one wholesale, as the game
// re-initializes the row, so a value the game never reads in the earlier block blocks
// nothing: the block is one ignored-input finding, and saving keeps it as the file has it.
static int replaced_action_block() {
	editor_test::TempProjectDir dir("opennova_catalog_replaced_test");
	TEST_EXPECT(editor_test::write_text(dir.file("weapon.def"),
		"weapon \"WPN_TWICE\"\naction \"FIRE\"\ndelayend nope\nend\naction \"FIRE\"\ndelayend 2\nend\nend\n"));
	DefCatalogDocument document; Diagnostic error;
	TEST_EXPECT(document.load(dir.file("weapon.def"), "weapon.def", AssetKind::WeaponDefs, "jo", error));
	TEST_EXPECT(!document.blocked() && document.issues().size() == 1 && !document.issues()[0].blocks &&
	            document.issues()[0].line == 2 && document.ignored_lines() == 1);
	const NodeAddress weapon{document.rows()[0]->id, node_kind(DefRecordKind::Weapon), 0};
	TEST_EXPECT(document.apply(field(weapon, "weaponweight", 1.5), error));
	TEST_EXPECT(document.save(error));
	std::string saved, message;
	TEST_EXPECT(opennova::io::read_file_text(dir.file("weapon.def"), saved, message));
	TEST_EXPECT(saved == editor_test::crlf("weapon \"WPN_TWICE\"\naction \"FIRE\"\ndelayend nope\nend\naction \"FIRE\"\ndelayend 2\nend\n"
	                                       "\tweaponweight 1.5\nend\n"));
	TEST_EXPECT(document.issues().size() == 1 && !document.blocked());
	return 0;
}
// "Go to" opens the catalog that defines a name at the record the graph's lookup reaches
// (S12 D3): an item by its id, the file read again once closed and the record found by the
// locator the graph read, its id shown; a locator the file does not have selects nothing.
static int go_to_record() {
	editor_test::TempProjectDir dir("opennova_catalog_goto_test");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(request::new_project(dir.file("project"), "Catalog"));
	session.run_operations();
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	session.handle(request::open_document("items.def"));
	auto *items = session.document_for("items.def"); TEST_EXPECT(items && !items->rows().empty());
	if (!items || items->rows().empty()) return 1;
	const int id = row_at(*items, 0).native.as<DefItemDef>().id;
	session.handle(request::close_document(items->path()));
	const GraphSymbol *item =
			view.findings.graph->resolve_symbol(ReferenceKind::Item, std::to_string(id));
	TEST_EXPECT(item && item->field == "id" && !item->locator.empty());
	if (!item) return 1;
	session.handle(request::open_document(item->file, item->locator, item->field));
	items = session.document_for("items.def");
	TEST_EXPECT(items && view.documents.active == items->path());
	TEST_EXPECT(items && view.documents.selection.primary.row == items->rows()[0]->id && view.documents.selection.primary.kind == node_kind(DefRecordKind::Item));
	TEST_EXPECT(editor_test::revealed_field(view) == "id");
	session.handle(request::open_document("weapon.def", "7"));
	TEST_EXPECT(session.document_for("weapon.def") != nullptr && view.documents.selection.primary.row == 0);
	return 0;
}
static int remove_last_item() {
    editor_test::TempProjectDir dir("opennova_catalog_empty_test");
    TEST_EXPECT(editor_test::write_text(dir.file("items.def"), "begin \"One\"\nid 100001\ntype marker\npcvehicle_spawnlist 8\nend\n"));
    DefCatalogDocument document; Diagnostic error;
    TEST_EXPECT(document.load(dir.file("items.def"), "items.def", AssetKind::ItemDefs, "jo", error));
    Edit remove; remove.operation = EditOperation::Remove;
    remove.address = {document.rows()[0]->id, node_kind(DefRecordKind::Item), 0};
    TEST_EXPECT(document.apply(remove, error));
    TEST_EXPECT(document.rows().empty() && document.spawn_ids().empty());
    TEST_EXPECT(document.save(error));
    document.undo();
    TEST_EXPECT(document.rows().size() == 1 && document.spawn_ids() == std::vector<int>({8}));
    document.redo(); TEST_EXPECT(!document.dirty());
    TEST_EXPECT(document.load(dir.file("items.def"), "items.def", AssetKind::ItemDefs, "jo", error));
    TEST_EXPECT(document.rows().empty() && !document.blocked());
    return 0;
}
// S11a: a field edit shows as changed and reverts; the spawn registry compares by its IDs
// (a slot set back to its own ID is no change, though the edit made a new state).
static int changes_since_save() {
	editor_test::TempProjectDir dir("opennova_catalog_changes_test");
	TEST_EXPECT(editor_test::write_text(dir.file("items.def"),
	                                    "begin \"One\"\nid 100001\ntype marker\nhp 10\npcvehicle_spawnlist 8\nend\n"));
	DefCatalogDocument document; Diagnostic error;
	TEST_EXPECT(document.load(dir.file("items.def"), "items.def", AssetKind::ItemDefs, "jo", error));
	const NodeAddress row{document.rows()[0]->id, node_kind(DefRecordKind::Item), 0};
	TEST_EXPECT(!document.field_changed(row, "hp") && document.record_change(row) == Document::RecordChange::Unchanged);
	TEST_EXPECT(document.apply(field(row, "hp", int64_t(20)), error));
	TEST_EXPECT(document.field_changed(row, "hp") && !document.field_changed(row, "id"));
	TEST_EXPECT(document.record_change(row) == Document::RecordChange::Changed);
	const std::vector<Edit> back = document.revert_edits(row, "hp");
	TEST_EXPECT(back.size() == 1 && back[0].operation == EditOperation::Set && std::get<int64_t>(back[0].value) == 10);
	TEST_EXPECT(document.apply(back, error) && !document.field_changed(row, "hp"));
	TEST_EXPECT(row_at(document, 0).native.as<DefItemDef>().hp == 10 && document.record_change(row) == Document::RecordChange::Unchanged);
	document.undo();
	TEST_EXPECT(row_at(document, 0).native.as<DefItemDef>().hp == 20 && document.field_changed(row, "hp"));
	Edit spawn; spawn.operation = EditOperation::SetFileValue; spawn.position = 0; spawn.value = int64_t(9);
	TEST_EXPECT(!document.file_state_changed() && document.apply(spawn, error) && document.file_state_changed());
	spawn.value = int64_t(8);
	TEST_EXPECT(document.apply(spawn, error) && !document.file_state_changed() && document.dirty());
	return 0;
}
// S12 D5: the Inspector's numbers are the file's. A speed reads in km/h, a slope in degrees
// (their units, the key the file writes and the parser's note on each), a set stores what the
// game's parser makes of the line, the saved file writes the number shown; a whole percent
// keeps to 0..100; the spawn slots are the file's registry's ids, the soldier and team
// filters offer the readers' tokens and take any other; a line with a present flag is an optional field
// (Clear leaves the line out); a round's light colour is a packed RGB; a husk piece's
// debris types are the table's names.
static int written_units() {
	editor_test::TempProjectDir dir("opennova_catalog_units_test");
	TEST_EXPECT(editor_test::write_text(dir.file("items.def"),
		"begin \"Buggy\"\nid 100001\ntype vehicle\nplayer_speed 30\nmax_slope 35\n"
		"light_transfer 35\npcvehicle_spawnlist 8 4\npcvehicle_spawnlist 4\nend\n"));
	DefCatalogDocument items; Diagnostic error;
	TEST_EXPECT(items.load(dir.file("items.def"), "items.def", AssetKind::ItemDefs, "jo", error));
	const NodeKind kItem = node_kind(DefRecordKind::Item);
	const NodeAddress buggy{items.rows()[0]->id, kItem, 0};
	// A field of the record's kind, as its type's table declares it.
	auto schema = [&](const Document &document, const NodeAddress &address, const char *id) -> const FieldSchema & {
		static const FieldSchema none;
		for (const FieldSchema &f : document.fields(address.kind))
			if (f.id == id) return f;
		return none;
	};
	Value value;
	TEST_EXPECT(items.get(buggy, "player_speed", value) && std::get<int64_t>(value) == 30);
	const FieldSchema speed = schema(items, buggy, "player_speed");
	TEST_EXPECT(speed.unit == "km/h" && speed.type == FieldType::Integer && speed.section == "Physics" &&
	            !speed.description.empty());
	TEST_EXPECT(items.get(buggy, "max_slope", value) && std::get<int64_t>(value) == 35 &&
	            schema(items, buggy, "max_slope").unit == "deg");
	// The demo round's bug 2: an item with no climb_speed line holds the allocation's word 1 [orig:
	// ItemDef_AllocateWithDefaults @0x49E3B0], no whole km/h: it shows 1/293 km/h, never the -2066861395 the
	// parser's 32-bit wrap reads back to the word; a set of what it shows is no edit.
	TEST_EXPECT(items.get(buggy, "climb_speed", value) && std::holds_alternative<double>(value) &&
	            std::get<double>(value) == 1.0 / 293.0 && field_text(schema(items, buggy, "climb_speed"), value) == "0.00341296928");
	const uint64_t before_climb = items.revision();
	TEST_EXPECT(items.apply(field(buggy, "climb_speed", value), error) && items.revision() == before_climb);
	const FieldSchema transfer = schema(items, buggy, "light_transfer");
	TEST_EXPECT(transfer.ranged && transfer.min == 0.0 && transfer.max == 100.0 && transfer.unit == "%");
	TEST_EXPECT(items.apply(field(buggy, "player_speed", int64_t(45)), error));
	TEST_EXPECT(row_at(items, 0).native.as<DefItemDef>().player_speed == 45 * 293);
	// (the file's line, its spelling and ending kept, the number changed)
	TEST_EXPECT(items.serialize().text.find("\nplayer_speed 45\r\n") != std::string::npos);
	// A Set of the number the line writes already is no edit (S12 Z2): no step.
	const uint64_t at_45 = items.revision();
	TEST_EXPECT(items.apply(field(buggy, "player_speed", int64_t(45)), error) && items.revision() == at_45);
	TEST_EXPECT(items.apply(field(buggy, "max_slope", int64_t(35)), error) && items.revision() == at_45);
	// The registry's ids name the mask's bits: the item's own choices (Document::record_choices),
	// none in the table.
	const FieldUse spawn = items.field_on(buggy, schema(items, buggy, "vehicle_spawn_mask"));
	std::vector<FieldChoice> own;
	const std::vector<FieldChoice> &bits = items.choices_on(buggy, spawn, own);
	TEST_EXPECT(spawn.schema->flags && spawn.own_choices && spawn.schema->choices.empty() && bits.size() == 2 &&
	            bits[0].name == "8" && bits[0].value == 1 && bits[1].name == "4" && bits[1].value == 2);
	const FieldSchema pieces = schema(items, buggy, "husk_sub_part_types[0]");
	TEST_EXPECT(pieces.choices.size() == 13 && pieces.choices[1].name == "WHEEL" && pieces.token == "husk_sub_part_types");

	TEST_EXPECT(editor_test::write_text(dir.file("weapon.def"),
		"weapon \"WPN_TEST\"\ncharfilter medic\nerror_hiptheta 0.5\nswitchcategory 3\nend\n"));
	DefCatalogDocument weapons;
	TEST_EXPECT(weapons.load(dir.file("weapon.def"), "weapon.def", AssetKind::WeaponDefs, "jo", error));
	const NodeAddress weapon{weapons.rows()[0]->id, node_kind(DefRecordKind::Weapon), 0};
	const FieldSchema filter = schema(weapons, weapon, "charfilter[0]");
	TEST_EXPECT(filter.open_choices && filter.choices.size() == 5 && filter.choices[4].name == "engineer");
	// The team tokens both readers take, and the loadout reader's yellow and violet.
	const FieldSchema team = schema(weapons, weapon, "teamfilter[0]");
	TEST_EXPECT(team.open_choices && team.choices.size() == 4 && team.choices[2].name == "yellow" &&
	            team.choices[2].value == 2 && team.choices[3].name == "violet" && team.choices[3].value == 1 &&
	            !team.description.empty());
	TEST_EXPECT(weapons.get(weapon, "error_hip_theta_fp16", value) && std::get<double>(value) == 0.5);
	const FieldSchema theta = schema(weapons, weapon, "error_hip_theta_fp16");
	TEST_EXPECT(theta.type == FieldType::Real && theta.token == "error_hiptheta" && theta.label == "Hip vertical spread");
	// switchcategory's line is written while its present flag is set: Clear leaves it out, its
	// latent value kept, and the file saves without the line and reads back without it. A Set of
	// another value writes it again (a Set of the value it holds does not), and Revert gives
	// back the saved value and presence.
	const FieldSchema category = schema(weapons, weapon, "switchcategory");
	TEST_EXPECT(category.optional && weapons.present(weapon, "switchcategory"));
	TEST_EXPECT(schema(weapons, weapon, "has_switchcategory").read_only);
	TEST_EXPECT(weapons.apply(field(weapon, "switchcategory", int64_t(4)), error));
	Edit clear; clear.operation = EditOperation::Clear; clear.address = weapon; clear.field = "switchcategory";
	TEST_EXPECT(weapons.apply(clear, error) && !weapons.present(weapon, "switchcategory"));
	TEST_EXPECT(weapons.apply(weapons.revert_edits(weapon, "switchcategory"), error));
	TEST_EXPECT(weapons.get(weapon, "switchcategory", value) && std::get<int64_t>(value) == 3 &&
	            weapons.present(weapon, "switchcategory"));
	TEST_EXPECT(weapons.apply(clear, error) && !weapons.present(weapon, "switchcategory"));
	TEST_EXPECT(weapons.apply(field(weapon, "switchcategory", int64_t(3)), error) && !weapons.present(weapon, "switchcategory"));
	TEST_EXPECT(weapons.apply(field(weapon, "switchcategory", int64_t(5)), error) && weapons.present(weapon, "switchcategory"));
	TEST_EXPECT(weapons.apply(clear, error));
	const SerializeResult written = weapons.serialize();
	TEST_EXPECT(written.ok() && !written.text.empty() && written.text.find("switchcategory") == std::string::npos);
	TEST_EXPECT(weapons.save(error));
	DefCatalogDocument reread;
	TEST_EXPECT(reread.load(dir.file("weapon.def"), "weapon.def", AssetKind::WeaponDefs, "jo", error));
	const NodeAddress again{reread.rows()[0]->id, node_kind(DefRecordKind::Weapon), 0};
	TEST_EXPECT(!reread.present(again, "switchcategory") && reread.get(again, "error_hip_theta_fp16", value) &&
	            std::get<double>(value) == 0.5);

	TEST_EXPECT(editor_test::write_text(dir.file("ammo.def"), "ammo AT_TEST\nmax_age 1.5\nlight_move 3 255 120 20\nend\n"));
	DefCatalogDocument ammo;
	TEST_EXPECT(ammo.load(dir.file("ammo.def"), "ammo.def", AssetKind::AmmoDefs, "jo", error));
	const NodeAddress round{ammo.rows()[0]->id, node_kind(DefRecordKind::Ammo), 0};
	TEST_EXPECT(ammo.get(round, "max_age_ticks", value) && std::get<double>(value) == 1.5 &&
	            schema(ammo, round, "max_age_ticks").unit == "s");
	TEST_EXPECT(ammo.apply(field(round, "max_age_ticks", 1.5), error) && !ammo.dirty() && !ammo.can_undo());
	TEST_EXPECT(schema(ammo, round, "light_move_color").color == FieldColor::PackedRgb &&
	            schema(ammo, round, "light_move_radius_fp16").label == "Flight light radius");
	return 0;
}

// S12 D10: the values the game's code tells apart, named by what it does with each (each table
// pinned here, value by value), an open list where a file may carry others the game reads; the
// integers no code singles out (a weapon's rank and run gait, a door's type) stay plain. The
// choices name values only: a file reads and writes as it did.
static int witnessed_enums() {
	editor_test::TempProjectDir dir("opennova_catalog_enums_test");
	using Table = std::vector<std::pair<int64_t, const char *>>;
	// A field of the record's kind, as its type's table declares it.
	auto schema = [](const Document &document, const NodeAddress &address, const char *id) -> const FieldSchema & {
		static const FieldSchema none;
		for (const FieldSchema &f : document.fields(address.kind))
			if (f.id == id) return f;
		return none;
	};
	auto pins = [](const FieldSchema &field, const Table &table, bool open) {
		if (field.choices.size() != table.size() || field.open_choices != open || field.flags) return false;
		for (size_t i = 0; i < table.size(); ++i)
			if (field.choices[i].value != table[i].first || field.choices[i].name != std::to_string(table[i].first) ||
			    field.choices[i].label != table[i].second)
				return false;
		return true;
	};
	Diagnostic error;
	const std::string weapon_text = "weapon \"WPN_ENUM\"\r\n\tcategory 11\r\n\trank 3\r\n\tspecial_hold 2\r\n"
	                                "\tattack_anim 1\r\n\trun_anim 1\r\nend\r\n\r\n";
	TEST_EXPECT(editor_test::write_text(dir.file("weapon.def"), weapon_text));
	DefCatalogDocument weapons;
	TEST_EXPECT(weapons.load(dir.file("weapon.def"), "weapon.def", AssetKind::WeaponDefs, "jo", error));
	const NodeAddress weapon{weapons.rows()[0]->id, node_kind(DefRecordKind::Weapon), 0};
	TEST_EXPECT(pins(schema(weapons, weapon, "category"),
	                 {{1, "Knife"}, {2, "Sidearm"}, {3, "Primary"}, {4, "Flashbang"}, {5, "Frag grenade"},
	                  {6, "Smoke grenade"}, {7, "Accessory"}, {8, "Detonator"}, {9, "Medpack"}, {11, "Mounted weapon"}},
	                 true));
	TEST_EXPECT(pins(schema(weapons, weapon, "special_hold"),
	                 {{0, "Rifle (the body's own state)"}, {1, "knife"}, {2, "pistol"}, {3, "grenade"}, {4, "stinger"},
	                  {5, "designator"}, {6, "P90"}, {7, "MP7"}, {8, "javelin"}},
	                 false));
	TEST_EXPECT(pins(schema(weapons, weapon, "attack_anim"), {{0, "Nothing"}, {1, "knife_attack"}, {2, "grenade_attack"}}, false));
	TEST_EXPECT(schema(weapons, weapon, "rank").choices.empty() && schema(weapons, weapon, "run_anim").choices.empty());
	Value value;
	TEST_EXPECT(weapons.get(weapon, "category", value) && std::get<int64_t>(value) == 11);
	// The ranges the game's readers keep: an edit past one refused (D10b).
	const FieldSchema category = schema(weapons, weapon, "category"), rank = schema(weapons, weapon, "rank");
	TEST_EXPECT(category.ranged && category.min == 0.0 && category.max == 11.0 && rank.ranged && rank.max == 64.0);
	Edit past;
	past.address = weapon;
	past.field = "category";
	past.value = int64_t(12);
	TEST_EXPECT(!weapons.apply(past, error));
	// Unchanged through the editor: the file writes back as it read.
	const std::string written = weapons.serialize().text;
	TEST_EXPECT(written.find("\tcategory 11\r\n") != std::string::npos && written.find("\trank 3\r\n") != std::string::npos &&
	            written.find("\tspecial_hold 2\r\n") != std::string::npos);

	TEST_EXPECT(editor_test::write_text(dir.file("items.def"),
		"begin \"Heli\"\nid 100010\ntype vehicle\nphysics 1\nunit_type 3\ndoor_type 2\nend\n"));
	DefCatalogDocument items;
	TEST_EXPECT(items.load(dir.file("items.def"), "items.def", AssetKind::ItemDefs, "jo", error));
	const NodeAddress heli{items.rows()[0]->id, node_kind(DefRecordKind::Item), 0};
	TEST_EXPECT(pins(schema(items, heli, "physics"), {{0, "Its class's motor"}, {1, "The physics block"}}, false));
	TEST_EXPECT(pins(schema(items, heli, "unit_type"),
	                 {{1, "Land vehicle"}, {2, "Land vehicle, TN voice set"}, {3, "Helicopter"}, {4, "Air vehicle"},
	                  {5, "Boat"}, {6, "Boat"}, {7, "Boat, medium fire"}, {8, "Boat, large fire"}, {9, "Own score tally"},
	                  {10, "Falling wreck"}, {11, "Bridge"}, {12, "Land vehicle, DB voice set"}},
	                 true));
	TEST_EXPECT(schema(items, heli, "door_type").choices.empty());
	TEST_EXPECT(items.get(heli, "unit_type", value) && std::get<int64_t>(value) == 3);
	TEST_EXPECT(schema(items, heli, "unit_type").ranged && schema(items, heli, "unit_type").max == 255.0);

	// A line the game reads as another number (D10b): the document holds what the game reads,
	// the line is reported and blocks nothing, and saving keeps the file's line while the record
	// holds what it read (the game reads it the same); a category set writes the new one.
	TEST_EXPECT(editor_test::write_text(dir.file("wide.def"), "weapon \"WPN_WIDE\"\ncategory 13\nend\n"));
	DefCatalogDocument wide;
	TEST_EXPECT(wide.load(dir.file("wide.def"), "weapon.def", AssetKind::WeaponDefs, "jo", error) && !wide.blocked());
	const NodeAddress wide_weapon{wide.rows()[0]->id, node_kind(DefRecordKind::Weapon), 0};
	TEST_EXPECT(wide.get(wide_weapon, "category", value) && std::get<int64_t>(value) == 0);
	TEST_EXPECT(wide.issues().size() == 1 && !wide.issues()[0].blocks && wide.issues()[0].field == "category" &&
	            wide.issues()[0].message.find("reads this as 0") != std::string::npos);
	TEST_EXPECT(wide.rewrite_need() == Document::RewriteNeed::None);
	TEST_EXPECT(wide.apply(field(wide_weapon, "category", int64_t(4)), error));
	TEST_EXPECT(wide.serialize().text == editor_test::crlf("weapon \"WPN_WIDE\"\ncategory 4\nend\n"));

	TEST_EXPECT(editor_test::write_text(dir.file("ammo.def"), "ammo AT_GLASS\nscar_type 2\nend\n"));
	DefCatalogDocument ammo;
	TEST_EXPECT(ammo.load(dir.file("ammo.def"), "ammo.def", AssetKind::AmmoDefs, "jo", error));
	const NodeAddress round{ammo.rows()[0]->id, node_kind(DefRecordKind::Ammo), 0};
	TEST_EXPECT(pins(schema(ammo, round, "scar_type"), {{0, "No mark"}, {2, "Glass only"}}, true));
	TEST_EXPECT(ammo.get(round, "scar_type", value) && std::get<int64_t>(value) == 2);
	return 0;
}

// A powerup row's weapon (S13 D10): `weapon all` and `weapon <name>` fill one word of the row, the later
// line's kept [orig: PowerUpDef_ParseProperty @0x4431A7..0x443216], so every weapon clears the name and
// a name clears every weapon; a weapon named all, in any case, would be read back as every weapon:
// refused. A row of a name an earlier row has is one no item binds.
static int powerup_weapon() {
	editor_test::TempProjectDir dir("opennova_catalog_powerup_test");
	TEST_EXPECT(editor_test::write_text(dir.file("powerup.def"),
	                                    "powerup \"PU_GUN\"\r\nweapon WPN_A\r\nend\r\n"
	                                    "powerup \"PU_TWICE\"\r\nweapon WPN_A\r\nweapon all\r\nend\r\n"));
	DefCatalogDocument document;
	Diagnostic error;
	TEST_EXPECT(document.load(dir.file("powerup.def"), "powerup.def", AssetKind::PowerupDefs, "jo", error));
	const NodeAddress gun{document.rows()[0]->id, node_kind(DefRecordKind::Powerup), 0};
	const NodeAddress twice{document.rows()[1]->id, node_kind(DefRecordKind::Powerup), 0};
	Value value;
	// The parse keeps the later line: `weapon all` after a name leaves no name behind it.
	TEST_EXPECT(document.get(twice, "weapon", value) && std::get<std::string>(value).empty() &&
	            document.get(twice, "weapon_all", value) && std::get<int64_t>(value) == 1);
	TEST_EXPECT(document.apply(field(gun, "weapon_all", int64_t(1)), error));
	TEST_EXPECT(document.get(gun, "weapon", value) && std::get<std::string>(value).empty());
	// The first row's line now `weapon all`; the second row's lines stand as the file has them (both).
	TEST_EXPECT(document.serialize().text == "powerup \"PU_GUN\"\r\nweapon all\r\nend\r\n"
	                                         "powerup \"PU_TWICE\"\r\nweapon WPN_A\r\nweapon all\r\nend\r\n");
	TEST_EXPECT(document.apply(field(gun, "weapon", std::string("WPN_B")), error));
	TEST_EXPECT(document.get(gun, "weapon_all", value) && std::get<int64_t>(value) == 0);
	const std::string text = document.serialize().text;
	TEST_EXPECT(text.find("weapon WPN_B") != std::string::npos && text.find("PU_GUN\"\r\n\tweapon all") == std::string::npos);
	for (const char *all : {"all", "ALL", "All"}) {
		TEST_EXPECT(!document.apply(field(gun, "weapon", std::string(all)), error) &&
		            error.message.find("every weapon") != std::string::npos);
	}
	TEST_EXPECT(document.get(gun, "weapon", value) && std::get<std::string>(value) == "WPN_B");
	// A second row of a name: the lookup by the name finds the first [orig: PowerUpDef_FindByName
	// @0x442660], so no item binds it.
	TEST_EXPECT(document.apply(field(twice, "name", std::string("pu_gun")), error));
	size_t repeated = 0;
	for (const Diagnostic &d : validate_catalog_file(document))
		repeated += d.code() == "catalog.name_duplicate" && d.message.find("no item binds this one") != std::string::npos &&
		            d.row_id == twice.row;
	TEST_EXPECT(repeated == 1);
	return 0;
}

// A Duplicate gives the copy an identity of its own, as an Add does (the UX audit's 4.4): an item a
// free id and its name with " (copy)", a weapon its token with "_2", each within the characters of a
// name the game keeps, never one a row of its kind has; nested records keep theirs.
static int duplicates_apart() {
	editor_test::TempProjectDir dir("opennova_catalog_duplicates_apart");
	const std::string long_name(44, 'L');
	TEST_EXPECT(editor_test::write_text(dir.file("items.def"),
	                                    "begin \"Dune Buggy\"\nid 100000\ntype vehicle\nhp 10\nend\nbegin \"Dune Buggy (copy)\"\n"
	                                    "id 100001\ntype vehicle\nend\nbegin \"" + long_name + "\"\nid 100003\ntype marker\nend\n"));
	DefCatalogDocument items; Diagnostic error;
	TEST_EXPECT(items.load(dir.file("items.def"), "items.def", AssetKind::ItemDefs, "jo", error));
	const NodeKind item = node_kind(DefRecordKind::Item);
	Edit duplicate; duplicate.operation = EditOperation::Duplicate; duplicate.address = {items.rows()[0]->id, item, 0};
	TEST_EXPECT(items.apply(duplicate, error));
	const auto &copy = static_cast<const CatalogRow &>(*items.row(items.last_added())).native.as<DefItemDef>();
	TEST_EXPECT(copy.id == 100002 && std::string(copy.display_name) == "Dune Buggy (copy 2)" && copy.hp == 10 &&
	            copy.type == DEF_ITEM_TYPE_VEHICLE);
	duplicate.address = {items.rows()[3]->id, item, 0}; // the long name's row, after the copy
	TEST_EXPECT(std::string(row_at(items, 3).native.as<DefItemDef>().display_name) == long_name);
	TEST_EXPECT(items.apply(duplicate, error));
	const auto &cut = static_cast<const CatalogRow &>(*items.row(items.last_added())).native.as<DefItemDef>();
	// The game keeps 46 characters of an item's name [orig: ItemDef_ParseProperty @ 0x49eb00, @0x49ebfb].
	TEST_EXPECT(cut.id == 100004 && std::string(cut.display_name) == std::string(39, 'L') + " (copy)");
	for (const Diagnostic &d : validate_catalog_file(items))
		TEST_EXPECT(d.code() != "catalog.name_duplicate" && d.code() != "catalog.item_identity");
	items.undo(); items.undo(); TEST_EXPECT(!items.dirty());

	TEST_EXPECT(editor_test::write_text(dir.file("weapon.def"), "weapon \"WPN_M16\"\naction \"FIRE\"\nend\nend\n"));
	DefCatalogDocument weapons;
	TEST_EXPECT(weapons.load(dir.file("weapon.def"), "weapon.def", AssetKind::WeaponDefs, "jo", error));
	duplicate.address = {weapons.rows()[0]->id, node_kind(DefRecordKind::Weapon), 0};
	TEST_EXPECT(weapons.apply(duplicate, error) && weapons.apply(duplicate, error));
	TEST_EXPECT(weapons.rows().size() == 3 && std::string(row_at(weapons, 1).native.as<DefWeaponDef>().weapon_name) == "WPN_M16_3" &&
	            std::string(row_at(weapons, 2).native.as<DefWeaponDef>().weapon_name) == "WPN_M16_2");
	const DefWeaponDef &gun = row_at(weapons, 2).native.as<DefWeaponDef>();
	TEST_EXPECT(gun.actions_count == 1 && std::string(gun.actions[0].name) == "FIRE");
	TEST_EXPECT(copy_name("WPN_ABCDEFGHIJKLMNOPQRSTUVWXYZ0", CopyName::Token, 31, {}) == "WPN_ABCDEFGHIJKLMNOPQRSTUVWXY_2");

	// A name of the game's code page cut by whole characters (the review's W2: by bytes it could end inside
	// one, the name then refused and the copy keeping its original's): 45 characters, two of them two
	// UTF-8 bytes, kept to 39 before " (copy)". An id other files name is no copy's (W9): with 100001 and
	// 100002 named elsewhere, the copy takes 100003.
	TEST_EXPECT(editor_test::write_text(dir.file("items.def"),
	                                    "begin \"V\xE9hicule blind\xE9 de transport de troupes lourd\"\nid 100000\ntype vehicle\nend\n"));
	DefCatalogDocument words;
	TEST_EXPECT(words.load(dir.file("items.def"), "items.def", AssetKind::ItemDefs, "jo", error));
	words.set_names_used_elsewhere(ReferenceKind::Item, {"100001", "100002", "not an id"});
	duplicate.address = {words.rows()[0]->id, item, 0};
	TEST_EXPECT(words.apply(duplicate, error));
	const auto &accented = static_cast<const CatalogRow &>(*words.row(words.last_added())).native.as<DefItemDef>();
	TEST_EXPECT(accented.id == 100003 &&
	            std::string(accented.display_name) == "V\xE9hicule blind\xE9 de transport de troupes (copy)");
	TEST_EXPECT(copy_name("\xC3\xA9\xC3\xA9\xC3\xA9", CopyName::Words, 9, {}) == "\xC3\xA9\xC3\xA9 (copy)");
	return 0;
}

// A table of names standing in for the project's (the graph's).
class Names : public NameSource {
public:
	void define(ReferenceKind kind, const std::string &name, const std::string &record, const std::string &value,
	            const std::string &scope = std::string()) {
		GraphSymbol symbol;
		symbol.kind = kind;
		symbol.name = symbol.display = name;
		symbol.record = record;
		symbol.value = value;
		symbol.scope = scope;
		symbols_.push_back(symbol);
	}
	const GraphSymbol *symbol(ReferenceKind kind, const std::string &name, const std::string &scope) const override {
		for (const GraphSymbol &symbol : symbols_)
			if (symbol.kind == kind && opennova::strutil::iequals(symbol.name, name) &&
			    (scope.empty() || opennova::strutil::iequals(symbol.scope, scope)))
				return &symbol;
		return nullptr;
	}
	const GraphSymbol *reached(const GraphEdge &) const override { return nullptr; }
	uint64_t generation() const override { return 1; }

private:
	std::vector<GraphSymbol> symbols_;
};

// The members in a modder's words (the UX round's plain-words lane, the audit's 4.1): every member of
// every kind a row in def_fields' order with a label, a section of its kind's, a meaning and its
// witness; the fields carry them, sorted by section (an item's identity first); a weapon titled by the
// name the HUD shows for it, else its loadout list's, an ammo by its round's, a mounted gun by its item,
// a powerup's lines by what they give and when.
static int plain_words() {
	for (size_t k = 0; k < kDefRecordKindCount; ++k) {
		const DefRecordKind kind = DefRecordKind(k);
		size_t count = 0;
		const DefWords *words = def_words(kind, count);
		const std::vector<DefField> &fields = def_fields(kind);
		TEST_EXPECT(words && count == fields.size());
		for (size_t i = 0; i < count && i < fields.size(); ++i) {
			const DefWords &row = words[i];
			TEST_EXPECT(fields[i].id == row.id);
			TEST_EXPECT(*row.label && *row.meaning && def_section_rank(kind, row.section) != SIZE_MAX);
			const std::string cite = row.cite;
			TEST_EXPECT(cite.find("[orig:") != std::string::npos || cite.find(".md") != std::string::npos);
			if (fields[i].id != row.id) std::printf("FAIL kind %zu member %zu: %s is not %s\n", k, i, row.id, fields[i].id.c_str());
		}
	}
	const std::vector<FieldSchema> &item = catalog_table().fields(node_kind(DefRecordKind::Item));
	TEST_EXPECT(item.front().section == "Identity");
	const FieldSchema *hp = nullptr;
	for (const FieldSchema &field : item)
		if (field.id == "hp") hp = &field;
	TEST_EXPECT(hp && hp->label == "Health" && hp->section == "Health and armour" &&
	            hp->description.find(def_words_of(DefRecordKind::Item, "hp")->meaning) == 0);
	// A choice a row gives words keeps its token (what the file writes) and shows its label, its
	// tooltip what the game does with it, cited: the shadow bits; the others show as their token.
	const auto choice_in = [&](const char *id, const char *token) -> const FieldChoice * {
		for (const FieldSchema &field : item)
			if (field.id == id)
				for (const FieldChoice &choice : field.choices)
					if (choice.name == token) return &choice;
		return nullptr;
	};
	const FieldChoice *no_shadow = choice_in("attrib", "noshadow");
	TEST_EXPECT(no_shadow && no_shadow->value == int64_t(DEF_ITEM_ATTRIB_NOSHADOW) && no_shadow->label == "No shadow" &&
	            no_shadow->description.find("no sun shadow") != std::string::npos &&
	            no_shadow->description.find("[orig: Terrain_CollectAndRenderTileModels @ 0x60D43E]") != std::string::npos);
	const FieldChoice *static_shadow = choice_in("attrib2", "staticshadow");
	const FieldChoice *dynamic_shadow = choice_in("attrib2", "dynamicshadow");
	TEST_EXPECT(static_shadow && static_shadow->label == "Static shadow" && !static_shadow->description.empty() &&
	            dynamic_shadow && dynamic_shadow->label == "Dynamic shadow" &&
	            dynamic_shadow->description.find("NoShadow does not") != std::string::npos);
	const FieldChoice *door = choice_in("attrib", "door");
	TEST_EXPECT(door && door->label.empty() && door->description.empty());
	TEST_EXPECT(field_text(*std::find_if(item.begin(), item.end(), [](const FieldSchema &f) { return f.id == "attrib"; }),
	                       int64_t(DEF_ITEM_ATTRIB_DOOR | DEF_ITEM_ATTRIB_NOSHADOW)) == "door, No shadow");
	// The sections in their kind's order, each once.
	size_t rank = 0;
	for (const FieldSchema &field : item) {
		const size_t at = def_section_rank(DefRecordKind::Item, field.section.c_str());
		TEST_EXPECT(at >= rank);
		rank = at;
	}

	editor_test::TempProjectDir dir("opennova_catalog_plain_words");
	TEST_EXPECT(editor_test::write_text(dir.file("weapon.def"), "weapon \"WPN_M16\"\nend\nweapon \"WPN_LOADOUT\"\n"
	                                                            "loadout_menu_textid WEP_LOAD\nend\nweapon \"WPN_BARE\"\nend\n"));
	DefCatalogDocument weapons; Diagnostic error;
	TEST_EXPECT(weapons.load(dir.file("weapon.def"), "weapon.def", AssetKind::WeaponDefs, "jo", error));
	Names names;
	const std::string wepdes = "GAMETEXT.BIN/WepDes";
	names.define(ReferenceKind::TextId, "WPN_M16", "", "M16A2 Rifle", wepdes);
	names.define(ReferenceKind::TextId, "WEP_LOAD", "", "Loadout Gun", wepdes);
	names.define(ReferenceKind::TextId, "AMMO_556", "", "5.56x45", wepdes);
	names.define(ReferenceKind::Item, "100184", "Hummer gun", "");
	const NodeKind weapon = node_kind(DefRecordKind::Weapon);
	const auto title = [&](const Document &document, size_t row, const NameSource *source) {
		return record_display(document, {document.rows()[row]->id, document.rows()[row]->kind, 0}, source);
	};
	TEST_EXPECT(title(weapons, 0, &names) == "M16A2 Rifle" && title(weapons, 1, &names) == "Loadout Gun" &&
	            title(weapons, 2, &names) == "WPN_BARE" && title(weapons, 0, nullptr) == "WPN_M16");
	TEST_EXPECT(weapons.rows()[0]->kind == weapon);
	TEST_EXPECT(editor_test::write_text(dir.file("ammo.def"), "ammo AMMO_556\nend\n"));
	DefCatalogDocument ammo;
	TEST_EXPECT(ammo.load(dir.file("ammo.def"), "ammo.def", AssetKind::AmmoDefs, "jo", error));
	TEST_EXPECT(title(ammo, 0, &names) == "5.56x45" && title(ammo, 0, nullptr) == "AMMO_556");
	TEST_EXPECT(editor_test::write_text(dir.file("items.def"),
	                                    "begin \"Hummer\"\nid 100100\ntype vehicle\naddeweap ewep01 100184\nend\n"));
	DefCatalogDocument items;
	TEST_EXPECT(items.load(dir.file("items.def"), "items.def", AssetKind::ItemDefs, "jo", error));
	const NodeId row = items.rows()[0]->id;
	const NodeAddress gun{row, node_kind(DefRecordKind::Attachment),
	                      items.collections_of({row, node_kind(DefRecordKind::Item), 0}).front().ids.front()};
	TEST_EXPECT(record_display(items, gun, &names) == "Hummer gun on ewep01" &&
	            record_display(items, gun, nullptr) == "Item 100184 on ewep01");
	TEST_EXPECT(title(items, 0, &names) == "Hummer");
	// A picker's words for a weapon: the HUD's name (definition_words over the weapon's symbol).
	GraphSymbol m16;
	m16.kind = ReferenceKind::Weapon;
	m16.display = "WPN_M16";
	TEST_EXPECT(definition_words(m16, &names) == "M16A2 Rifle" && definition_words(m16, nullptr) == "WPN_M16");
	return 0;
}

// A def's names are the game's code page (Windows-1252), the editor's UTF-8 (the plain-words lane, the
// audit's 9.1): a weapon named "Caf\xE9" in the file reads as "Café", the item naming it reaches it in the
// graph (both read in the one encoding), a name set is written back in the code page, and one the code
// page has no character of is refused.
static int code_page_names() {
	editor_test::TempProjectDir dir("opennova_catalog_code_page");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Code page"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	const AssetEntry *weapon_entry = view.project.scan->find("weapon.def");
	const AssetEntry *item_entry = view.project.scan->find("items.def");
	TEST_EXPECT(weapon_entry && item_entry);
	if (!weapon_entry || !item_entry) return 1;
	const std::string weapon_file = weapon_entry->relative_path, item_file = item_entry->relative_path;
	TEST_EXPECT(editor_test::write_text(view.project.root + "/" + weapon_file, "weapon \"Caf\xE9\"\nend\n") &&
	            editor_test::write_text(view.project.root + "/" + item_file,
	                                    "begin \"Thing\"\nid 100300\nprimary_weapon Caf\xE9\nend\n"));
	editor_test::handle_to_end(session, request::rescan());
	TEST_EXPECT(view.findings.graph != nullptr);
	if (!view.findings.graph) return 1;
	const AssetGraph &graph = *view.findings.graph;
	TEST_EXPECT(graph.symbols_named(ReferenceKind::Weapon, "Caf\xC3\xA9").size() == 1);
	bool reached = false;
	for (const GraphEdge *edge : graph.references_of(item_file))
		if (edge->field == "primary_weapon")
			reached = edge->value == "Caf\xC3\xA9" && graph.resolve(*edge) == ReferenceStatus::Present;
	TEST_EXPECT(reached);
	DefCatalogDocument weapons;
	Diagnostic error;
	TEST_EXPECT(weapons.load(view.project.root + "/" + weapon_file, weapon_file, AssetKind::WeaponDefs, "jo", error));
	const NodeAddress gun{weapons.rows()[0]->id, weapons.rows()[0]->kind, 0};
	Value name;
	TEST_EXPECT(weapons.get(gun, "weapon_name", name) && std::get<std::string>(name) == "Caf\xC3\xA9");
	TEST_EXPECT(weapons.apply(field(gun, "weapon_name", std::string("Caf\xC3\xA9 2")), error) &&
	            weapons.serialize().text.find("\"Caf\xE9 2\"") != std::string::npos);
	TEST_EXPECT(!weapons.apply(field(gun, "weapon_name", std::string("\xE6\x97\xA5")), error) &&
	            error.message.find("Windows-1252") != std::string::npos);
	return 0;
}

// An ordinary edit through the session keeps the file's order (the review's W5): a weapon's switchcategory
// unticked (its line's present tick cleared) and saved is that one line gone, every other line as the save
// before it wrote it, no record reordered and no note.
static int unticked_line_saves_alone() {
	editor_test::TempProjectDir dir("opennova_catalog_unticked_line");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Unticked"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	const AssetEntry *entry = view.project.scan->find("weapon.def");
	TEST_EXPECT(entry != nullptr);
	if (!entry) return 1;
	const std::string path = entry->relative_path, file = view.project.root + "/" + entry->relative_path;
	TEST_EXPECT(editor_test::write_text(file, "weapon \"WPN_A\"\n\tcategory 2\n\tswitchcategory 3\n\tclipsize 15\nend\n"
	                                          "weapon \"WPN_B\"\n\tclipsize 30\n\tcategory 1\nend\n"));
	editor_test::handle_to_end(session, request::rescan());
	editor_test::handle_to_end(session, request::open_document(path));
	Document *weapons = session.document_for(path);
	TEST_EXPECT(weapons != nullptr);
	if (!weapons) return 1;
	const std::string before = weapons->serialize().text;
	Edit untick;
	untick.operation = EditOperation::Clear;
	untick.address = {weapons->rows()[0]->id, weapons->rows()[0]->kind, 0};
	untick.field = "switchcategory";
	editor_test::handle_to_end(session, request::edit_record(path, untick));
	editor_test::handle_to_end(session, request::save(path));
	const std::vector<uint8_t> saved = test_io::read_file(file);
	std::string after(saved.begin(), saved.end());
	// The document unedited writes the file as it was read (its notes, def_notes.h: its CR LF endings too,
	// the one break the retail walk splits at, which write_text staged it with).
	TEST_EXPECT(before == "weapon \"WPN_A\"\r\n\tcategory 2\r\n\tswitchcategory 3\r\n\tclipsize 15\r\nend\r\n"
	                      "weapon \"WPN_B\"\r\n\tclipsize 30\r\n\tcategory 1\r\nend\r\n");
	const size_t at = before.find("\tswitchcategory 3\r\n");
	TEST_EXPECT(at != std::string::npos && after == std::string(before).erase(at, std::strlen("\tswitchcategory 3\r\n")));
	TEST_EXPECT(view.activity.status == "Saved 1 file." && weapons->save_notes().empty());
	return 0;
}

// A name the reader cuts takes any length, as the game's line does, and holds what the game keeps: retail's
// items.def names its map centre "Map Centerpoint, helps align commander map grid", 47 characters, of
// which the begin arm keeps 46 [orig: ItemDef_ParseProperty @0x49EBD9, the cut @0x49EBFB]; the saved line
// reads back the same. A text the reader does not cut is refused past its field, saying how much it holds.
static int item_name_cut() {
	editor_test::TempProjectDir dir("opennova_catalog_item_name_cut");
	TEST_EXPECT(editor_test::write_text(dir.file("items.def"), "begin \"Map centre\"\nid 102043\ntype marker\nend\n"));
	DefCatalogDocument items; Diagnostic error;
	TEST_EXPECT(items.load(dir.file("items.def"), "items.def", AssetKind::ItemDefs, "jo", error));
	const NodeAddress item{items.rows()[0]->id, node_kind(DefRecordKind::Item), 0};
	const std::string retail = "Map Centerpoint, helps align commander map grid";
	TEST_EXPECT(retail.size() == 47);
	TEST_EXPECT(items.apply(field(item, "display_name", retail), error));
	TEST_EXPECT(std::string(row_at(items, 0).native.as<DefItemDef>().display_name) == retail.substr(0, 46));
	TEST_EXPECT(items.save(error));
	DefCatalogDocument again;
	TEST_EXPECT(again.load(dir.file("items.def"), "items.def", AssetKind::ItemDefs, "jo", error));
	TEST_EXPECT(std::string(row_at(again, 0).native.as<DefItemDef>().display_name) == retail.substr(0, 46));
	// The read-back file's line as the game reads it: 46 kept of any longer name.
	TEST_EXPECT(editor_test::write_text(dir.file("items.def"), "begin \"" + retail + "\"\nid 102043\ntype marker\nend\n"));
	DefCatalogDocument shipped;
	TEST_EXPECT(shipped.load(dir.file("items.def"), "items.def", AssetKind::ItemDefs, "jo", error));
	TEST_EXPECT(std::string(row_at(shipped, 0).native.as<DefItemDef>().display_name) == retail.substr(0, 46));
	// The text id's member holds what its line names, uncut: past it, refused.
	const size_t text_id = sizeof(DefItemDef::text_id);
	TEST_EXPECT(!items.apply(field(item, "text_id", std::string(text_id, 'T')), error));
	TEST_EXPECT(error.message.find("holds " + std::to_string(text_id - 1) + " characters") != std::string::npos);
	return 0;
}

// The file's ending stays as it is however many items an Add puts at its end: the blank line after
// the last record (the writer's own form, and the base game's file) is one blank line after each
// add, never one more per add.
static int adds_keep_the_ending() {
	editor_test::TempProjectDir dir("opennova_catalog_adds_keep_the_ending");
	const std::string text = "// Item definitions\r\n\r\nbegin \"A\"\r\nid 100000\r\ntype marker\r\nend\r\n\r\n";
	TEST_EXPECT(editor_test::write_text(dir.file("items.def"), text));
	DefCatalogDocument items; Diagnostic error;
	TEST_EXPECT(items.load(dir.file("items.def"), "items.def", AssetKind::ItemDefs, "jo", error));
	TEST_EXPECT(items.serialize().text == text);
	const auto ending = [](const std::string &file) {
		size_t blank = 0;
		for (size_t at = file.size(); at >= 4 && file.compare(at - 4, 4, "\r\n\r\n") == 0; at -= 2) ++blank;
		return blank;
	};
	for (int n = 0; n < 3; ++n) {
		Edit add; add.operation = EditOperation::Add; add.address.kind = node_kind(DefRecordKind::Item);
		add.field = "display_name"; add.value = std::string("Item ") + char('B' + n);
		TEST_EXPECT(items.apply(add, error));
		const std::string written = items.serialize().text;
		if (ending(written) != 1) {
			std::string shown;
			for (const char c : written) shown += c == '\r' ? std::string("<CR>") : std::string(1, c);
			std::printf("after %d adds:\n%s|EOF\n", n + 1, shown.c_str());
		}
		TEST_EXPECT(ending(written) == 1);
	}
	return 0;
}

// The type line's word where the game reads two alike (foliage for 2, object for 6, as retail's trees and
// crates write them [orig: ItemDef_ParseProperty, the type chain @0x4A02E4..0x4A04B7]): read, kept by a
// copy (written in the writer's form), set on a new item, and dropped for a kind with one word.
static int type_words() {
	editor_test::TempProjectDir dir("opennova_catalog_type_words");
	TEST_EXPECT(editor_test::write_text(dir.file("items.def"),
	                                    "begin \"Jungle Bush\"\r\nid 100300\r\ntype foliage\r\nend\r\n\r\n"
	                                    "begin \"Crate\"\r\nid 100301\r\ntype object\r\nend\r\n\r\n"));
	DefCatalogDocument items; Diagnostic error;
	TEST_EXPECT(items.load(dir.file("items.def"), "items.def", AssetKind::ItemDefs, "jo", error));
	const auto &bush = row_at(items, 0).native.as<DefItemDef>();
	TEST_EXPECT(bush.type == DEF_ITEM_TYPE_FOLIAGE && bush.type_word == 1 && row_at(items, 1).native.as<DefItemDef>().type_word == 1);
	const NodeKind item = node_kind(DefRecordKind::Item);
	Edit duplicate; duplicate.operation = EditOperation::Duplicate; duplicate.address = {items.rows()[0]->id, item, 0};
	TEST_EXPECT(items.apply(duplicate, error));
	// The record's lines from its header to its end, as written.
	const auto lines_of = [&items](const char *header) {
		const std::string text = items.serialize().text;
		const size_t at = text.find(header);
		return at == std::string::npos ? std::string() : text.substr(at, text.find("end\r\n", at) - at);
	};
	TEST_EXPECT(lines_of("begin \"Jungle Bush (copy)\"").find("\ttype foliage\r\n") != std::string::npos);
	// A new item made foliage: its type 2, written as the other word.
	Edit add; add.operation = EditOperation::Add; add.address.kind = item; add.field = "display_name"; add.value = std::string("Palm");
	TEST_EXPECT(items.apply(add, error));
	const NodeAddress palm{items.last_added(), item, 0};
	TEST_EXPECT(items.apply(std::vector<Edit>{field(palm, "type", int64_t(DEF_ITEM_TYPE_DECORATION)), field(palm, "type_word", int64_t(1))},
	                        error));
	TEST_EXPECT(lines_of("begin \"Palm\"").find("\ttype foliage\r\n") != std::string::npos);
	// Made a vehicle: one word, the other dropped.
	TEST_EXPECT(items.apply(field(palm, "type", int64_t(DEF_ITEM_TYPE_VEHICLE)), error));
	TEST_EXPECT(static_cast<const CatalogRow &>(*items.row(palm.row)).native.as<DefItemDef>().type_word == 0);
	return 0;
}

int main() {
	return type_words() || adds_keep_the_ending() || item_name_cut() || unticked_line_saves_alone() || code_page_names() || history_and_save() || two_new_items() || collections() ||
	       nested_rows_follow_the_record() || session_gate() || malformed() || ignored_input() ||
	       replaced_action_block() || go_to_record() || remove_last_item() || changes_since_save() || written_units() ||
	       witnessed_enums() || powerup_weapon() || duplicates_apart() || plain_words();
}
