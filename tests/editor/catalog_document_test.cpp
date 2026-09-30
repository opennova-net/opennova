#include <editor/documents/def_catalog_document.h>
#include <editor/documents/catalog_validation.h>
#include <editor/graph/asset_graph.h>
#include <editor/session/project_session.h>
#include <editor/project/project_files.h>
#include "editor/editor_test_support.h"
#include "common/test_expect.h"
#include <cstring>

using namespace opennova::editor;
using namespace opennova::def;

struct NoProcess : ProcessPlatform {
	int spawns = 0;
	int64_t spawn(const LaunchPlan &) override { ++spawns; return -1; }
	bool is_running(int64_t) override { return false; }
	bool terminate(int64_t) override { return true; }
	bool kill(int64_t) override { return true; }
	void release(int64_t) override {}
	int64_t now_ms() override { return 0; }
	void sleep_ms(int64_t) override {}
};

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
	TEST_EXPECT(std::get<DefItemDef>(row_at(document, 0).data).hp == 10);
	document.redo();
	TEST_EXPECT(std::get<DefItemDef>(row_at(document, 0).data).hp == 25);
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
	TEST_EXPECT(read_file_text(dir.file("items.def"), external, message));
	const auto hp = external.find("hp 25"); TEST_EXPECT(hp != std::string::npos);
	external.replace(hp, 5, "hp 99");
	TEST_EXPECT(editor_test::write_text(dir.file("items.def"), external));
	std::filesystem::last_write_time(dir.file("items.def"), stamp);
	TEST_EXPECT(!document.save(error) && error.code == "document.conflict");
	std::string retained; TEST_EXPECT(read_file_text(dir.file("items.def"), retained, message));
	TEST_EXPECT(retained == external && document.dirty());

	DefCatalogDocument reloaded;
	TEST_EXPECT(reloaded.load(dir.file("items.def"), "items.def", AssetKind::ItemDefs, "jo", error));
	TEST_EXPECT(std::get<DefItemDef>(row_at(reloaded, 0).data).hp == 99);
	TEST_EXPECT(reloaded.apply(field(row, "hp", int64_t(7)), error));
	std::filesystem::create_directory(dir.file("items.def.tmp"));
	TEST_EXPECT(!reloaded.save(error) && error.code == "document.write" && reloaded.dirty());
	TEST_EXPECT(read_file_text(dir.file("items.def"), retained, message) && retained == external);
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
	TEST_EXPECT(held.size() == 2 && std::string(held[0].spec.kind_name) == "action" && held[0].ids == std::vector<NodeId>{child} &&
	            std::string(held[1].spec.kind_name) == "sight" && held[1].ids.empty());
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
	TEST_EXPECT(std::get<DefWeaponDef>(row_at(document, 0).data).weaponweight_fp16 == 81920);
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
static int session_gate() {
	editor_test::TempProjectDir dir("opennova_catalog_session_test");
	NoProcess platform;
	ProjectSession session(platform, dir.file("settings.json"));
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Catalog"));
	editor_test::create_missing_files(session);
    session.handle(make_request(EditorRequestKind::CreateFile, "ammo.def"));
    TEST_EXPECT(session.document_for("ammo.def"));
    session.handle(make_request(EditorRequestKind::CreateFile, "ammo.def"));
    TEST_EXPECT(session.document_for("ammo.def")->rows().size() == 1);
	session.handle(make_request(EditorRequestKind::OpenDocument, "items.def"));
	auto *document = session.document_for(); TEST_EXPECT(document);
	const auto id = document->rows()[0]->id;
	auto request = make_request(EditorRequestKind::EditRecord);
	request.edit = field({id, node_kind(DefRecordKind::Item), 0}, "graphic", std::string("missing"));
	session.handle(request);
	// Play packs the files on disk: it waits on the prompt, which lists the edited catalog
	// and offers no Discard; its Save writes the catalog, then Play builds.
	session.handle(make_request(EditorRequestKind::Play));
	const SessionView::UnsavedPrompt &prompt = session.view().unsaved_prompt;
	TEST_EXPECT(platform.spawns == 0 && !session.view().operation.running() && prompt.open && prompt.action == EditorRequestKind::Play);
	TEST_EXPECT(prompt.files == std::vector<std::string>{document->path()} && !prompt.can_discard);
	auto answer = make_request(EditorRequestKind::ResolveUnsaved); answer.unsaved_choice = UnsavedChoice::Save;
	session.handle(answer); session.run_operations();
	TEST_EXPECT(!document->dirty() && !prompt.open); // semantic errors do not prevent saving
	TEST_EXPECT(session.view().has_build && !session.view().last_build.ok && platform.spawns == 0); // the graphic is missing
	request.edit = field({id, node_kind(DefRecordKind::Item), 0}, "graphic", std::string(""));
	session.handle(request);
	session.handle(make_request(EditorRequestKind::CloseProject));
	TEST_EXPECT(session.project_open() && prompt.open && prompt.can_discard);
	answer.unsaved_choice = UnsavedChoice::Cancel;
	session.handle(answer); TEST_EXPECT(session.project_open() && !prompt.open);
	session.handle(make_request(EditorRequestKind::CloseProject));
	answer.unsaved_choice = UnsavedChoice::Save;
	session.handle(answer); TEST_EXPECT(!session.project_open());
	session.handle(make_request(EditorRequestKind::OpenProject, dir.file("project")));
	session.handle(make_request(EditorRequestKind::Build)); session.run_operations();
	TEST_EXPECT(session.view().last_build.ok);
	session.handle(make_request(EditorRequestKind::OpenDocument, "items.def"));
	document = session.document_for(); TEST_EXPECT(document && !document->dirty());
	return 0;
}
static int malformed() {
	editor_test::TempProjectDir dir("opennova_catalog_malformed_test");
	TEST_EXPECT(editor_test::write_text(dir.file("items.def"), "begin \"Bad\"\nhp twelve\nend\n"));
	DefCatalogDocument document; Diagnostic error;
	TEST_EXPECT(document.load(dir.file("items.def"), "items.def", AssetKind::ItemDefs, "jo", error));
	TEST_EXPECT(document.blocked() && document.issues().front().line == 2);
	TEST_EXPECT(!document.save(error) && error.code == "document.unserializable");
	TEST_EXPECT(editor_test::write_text(dir.file("items.def"), "begin \"Open\"\nid 100001\n"));
	TEST_EXPECT(document.load(dir.file("items.def"), "items.def", AssetKind::ItemDefs, "jo", error));
	TEST_EXPECT(document.blocked());
	return 0;
}
// Lines the game ignores never block the document: they are reported, the record
// stays editable, and saving drops them.
static int ignored_input() {
	editor_test::TempProjectDir dir("opennova_catalog_ignored_test");
	TEST_EXPECT(editor_test::write_text(dir.file("items.def"),
		"begin \"One\"\nid 100001\ntype marker\nsubtype Ruins\nattrib: good nodie\nend\n"));
	DefCatalogDocument document; Diagnostic error;
	TEST_EXPECT(document.load(dir.file("items.def"), "items.def", AssetKind::ItemDefs, "jo", error));
	TEST_EXPECT(!document.blocked() && document.ignored_lines() == 2 && document.issues().size() == 2);
	const NodeAddress row{document.rows()[0]->id, node_kind(DefRecordKind::Item), 0};
	TEST_EXPECT(document.apply(field(row, "hp", int64_t(20)), error));
	TEST_EXPECT(document.save(error));
	// The findings are the written text's from then on: the dropped lines are gone.
	TEST_EXPECT(document.ignored_lines() == 0 && document.issues().empty() && document.can_undo());
	std::string saved, message;
	TEST_EXPECT(read_file_text(dir.file("items.def"), saved, message));
	TEST_EXPECT(saved.find("subtype") == std::string::npos && saved.find("good") == std::string::npos);
	TEST_EXPECT(saved.find("nodie") != std::string::npos && saved.find("hp 20") != std::string::npos);
	TEST_EXPECT(document.load(dir.file("items.def"), "items.def", AssetKind::ItemDefs, "jo", error));
	TEST_EXPECT(document.ignored_lines() == 0 && document.issues().empty());
	TEST_EXPECT((std::get<DefItemDef>(row_at(document, 0).data).attrib & DEF_ITEM_ATTRIB_NODIE) != 0);
	return 0;
}
// A later action block of a name replaces the earlier one wholesale, as the game
// re-initializes the row, so a value the game never reads in the earlier block blocks
// nothing: the block is one ignored-input finding, and saving drops it.
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
	TEST_EXPECT(read_file_text(dir.file("weapon.def"), saved, message));
	TEST_EXPECT(saved.find("nope") == std::string::npos && saved.find("delayend 2") != std::string::npos);
	TEST_EXPECT(document.issues().empty() && !document.blocked());
	return 0;
}
// "Go to" opens the catalog that defines a name at the record the graph's lookup reaches
// (S12 D3): an item by its id, the file read again once closed and the record found by the
// locator the graph read, its id shown; a locator the file does not have selects nothing.
static int go_to_record() {
	editor_test::TempProjectDir dir("opennova_catalog_goto_test");
	NoProcess platform;
	ProjectSession session(platform, dir.file("settings.json"));
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Catalog"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	session.handle(make_request(EditorRequestKind::OpenDocument, "items.def"));
	auto *items = session.document_for("items.def"); TEST_EXPECT(items && !items->rows().empty());
	if (!items || items->rows().empty()) return 1;
	const int id = std::get<DefItemDef>(row_at(*items, 0).data).id;
	session.handle(make_request(EditorRequestKind::CloseDocument, items->path()));
	const GraphSymbol *item = view.graph->resolve_symbol(ReferenceKind::Item, std::to_string(id));
	TEST_EXPECT(item && item->field == "id" && !item->locator.empty());
	if (!item) return 1;
	EditorRequest open = make_request(EditorRequestKind::OpenDocument, item->file, item->locator);
	open.edit.field = item->field;
	session.handle(open);
	items = session.document_for("items.def");
	TEST_EXPECT(items && view.active_document == items->path());
	TEST_EXPECT(items && view.selection.row == items->rows()[0]->id && view.selection.kind == node_kind(DefRecordKind::Item));
	TEST_EXPECT(view.reveal_field == "id");
	session.handle(make_request(EditorRequestKind::OpenDocument, "weapon.def", "7"));
	TEST_EXPECT(session.document_for("weapon.def") != nullptr && view.selection.row == 0);
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
	TEST_EXPECT(std::get<DefItemDef>(row_at(document, 0).data).hp == 10 && document.record_change(row) == Document::RecordChange::Unchanged);
	document.undo();
	TEST_EXPECT(std::get<DefItemDef>(row_at(document, 0).data).hp == 20 && document.field_changed(row, "hp"));
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
	auto schema = [&](const Document &document, const NodeAddress &address, const char *id) {
		for (const FieldSchema &f : document.fields(address.kind))
			if (f.id == id) return document.field_on(address, f);
		return FieldSchema();
	};
	Value value;
	TEST_EXPECT(items.get(buggy, "player_speed", value) && std::get<int64_t>(value) == 30);
	const FieldSchema speed = schema(items, buggy, "player_speed");
	TEST_EXPECT(speed.unit == "km/h" && speed.type == FieldType::Integer && speed.section == "Physics" &&
	            !speed.description.empty());
	TEST_EXPECT(items.get(buggy, "max_slope", value) && std::get<int64_t>(value) == 35 &&
	            schema(items, buggy, "max_slope").unit == "deg");
	const FieldSchema transfer = schema(items, buggy, "light_transfer");
	TEST_EXPECT(transfer.ranged && transfer.min == 0.0 && transfer.max == 100.0 && transfer.unit == "%");
	TEST_EXPECT(items.apply(field(buggy, "player_speed", int64_t(45)), error));
	TEST_EXPECT(std::get<DefItemDef>(row_at(items, 0).data).player_speed == 45 * 293);
	TEST_EXPECT(items.serialize().text.find("\tplayer_speed 45\r\n") != std::string::npos);
	// A Set of the number the line writes already is no edit (S12 Z2): no step.
	const uint64_t at_45 = items.revision();
	TEST_EXPECT(items.apply(field(buggy, "player_speed", int64_t(45)), error) && items.revision() == at_45);
	TEST_EXPECT(items.apply(field(buggy, "max_slope", int64_t(35)), error) && items.revision() == at_45);
	// The registry's ids name the mask's bits.
	const FieldSchema spawn = schema(items, buggy, "vehicle_spawn_mask");
	TEST_EXPECT(spawn.flags && spawn.choices.size() == 2 && spawn.choices[0].name == "8" && spawn.choices[0].value == 1 &&
	            spawn.choices[1].name == "4" && spawn.choices[1].value == 2);
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
	TEST_EXPECT(theta.type == FieldType::Real && theta.token == "error_hiptheta" && theta.label == "error_hiptheta");
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
	            schema(ammo, round, "light_move_radius_fp16").label == "Radius");
	return 0;
}

// S12 D10: the values the game's code tells apart, named by what it does with each (each table
// pinned here, value by value), an open list where a file may carry others the game reads; the
// integers no code singles out (a weapon's rank and run gait, a door's type) stay plain. The
// choices name values only: a file reads and writes as it did.
static int witnessed_enums() {
	editor_test::TempProjectDir dir("opennova_catalog_enums_test");
	using Table = std::vector<std::pair<int64_t, const char *>>;
	auto schema = [](const Document &document, const NodeAddress &address, const char *id) {
		for (const FieldSchema &f : document.fields(address.kind))
			if (f.id == id) return document.field_on(address, f);
		return FieldSchema();
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
	// the line is reported and blocks nothing, and saving writes what the game reads.
	TEST_EXPECT(editor_test::write_text(dir.file("wide.def"), "weapon \"WPN_WIDE\"\ncategory 13\nend\n"));
	DefCatalogDocument wide;
	TEST_EXPECT(wide.load(dir.file("wide.def"), "weapon.def", AssetKind::WeaponDefs, "jo", error) && !wide.blocked());
	const NodeAddress wide_weapon{wide.rows()[0]->id, node_kind(DefRecordKind::Weapon), 0};
	TEST_EXPECT(wide.get(wide_weapon, "category", value) && std::get<int64_t>(value) == 0);
	TEST_EXPECT(wide.issues().size() == 1 && !wide.issues()[0].blocks && wide.issues()[0].field == "category" &&
	            wide.issues()[0].message.find("reads this as 0") != std::string::npos);
	TEST_EXPECT(wide.rewrite_need() == Document::RewriteNeed::Rewrite);

	TEST_EXPECT(editor_test::write_text(dir.file("ammo.def"), "ammo AT_GLASS\nscar_type 2\nend\n"));
	DefCatalogDocument ammo;
	TEST_EXPECT(ammo.load(dir.file("ammo.def"), "ammo.def", AssetKind::AmmoDefs, "jo", error));
	const NodeAddress round{ammo.rows()[0]->id, node_kind(DefRecordKind::Ammo), 0};
	TEST_EXPECT(pins(schema(ammo, round, "scar_type"), {{0, "No mark"}, {2, "Glass only"}}, true));
	TEST_EXPECT(ammo.get(round, "scar_type", value) && std::get<int64_t>(value) == 2);
	return 0;
}

int main() {
	return history_and_save() || collections() || session_gate() || malformed() || ignored_input() ||
	       replaced_action_block() || go_to_record() || remove_last_item() || changes_since_save() || written_units() ||
	       witnessed_enums();
}
