#include <editor/documents/editable_document.h>
#include <editor/documents/catalog_validation.h>
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

static CatalogEdit field(CatalogAddress address, std::string key, DefValue value, bool coalesce = false) {
	CatalogEdit edit; edit.address = address; edit.field = std::move(key);
	edit.value = std::move(value); edit.coalesce = coalesce; return edit;
}
static int history_and_save() {
	editor_test::TempProjectDir dir("opennova_catalog_document_test");
	TEST_EXPECT(editor_test::write_text(dir.file("items.def"), "begin \"One\"\nid 100001\ntype marker\nhp 10\nend\n"));
	EditableDocument document; Diagnostic error;
	TEST_EXPECT(document.load(dir.file("items.def"), "items.def", AssetKind::ItemDefs, "jo", error));
	const auto id = document.rows()[0]->id;
	const CatalogAddress row{id, DefRecordKind::Item, 0};
	TEST_EXPECT(document.apply(field(row, "hp", int64_t(20), true), error));
	TEST_EXPECT(document.apply(field(row, "hp", int64_t(25), true), error));
	TEST_EXPECT(document.dirty());
	document.undo();
	TEST_EXPECT(!document.dirty());
	TEST_EXPECT(std::get<DefItemDef>(document.rows()[0]->data).hp == 10);
	document.redo();
	TEST_EXPECT(std::get<DefItemDef>(document.rows()[0]->data).hp == 25);
	TEST_EXPECT(document.save(error));
	TEST_EXPECT(!document.dirty());
	document.undo(); TEST_EXPECT(document.dirty());
	document.redo(); TEST_EXPECT(!document.dirty());
	TEST_EXPECT(document.apply(field(row, "graphic", std::string("missing_model")), error));
	TEST_EXPECT(document.save(error)); // a serializable draft with a missing dependency

	CatalogEdit duplicate; duplicate.address = row; duplicate.operation = CatalogOperation::Duplicate;
	TEST_EXPECT(document.apply(duplicate, error)); const auto duplicate_id = document.last_added();
	TEST_EXPECT(duplicate_id != id && document.rows().size() == 2);
	duplicate.operation = CatalogOperation::Move; duplicate.address.row = duplicate_id; duplicate.position = 0;
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

	EditableDocument reloaded;
	TEST_EXPECT(reloaded.load(dir.file("items.def"), "items.def", AssetKind::ItemDefs, "jo", error));
	TEST_EXPECT(std::get<DefItemDef>(reloaded.rows()[0]->data).hp == 99);
	TEST_EXPECT(reloaded.apply(field(row, "hp", int64_t(7)), error));
	std::filesystem::create_directory(dir.file("items.def.tmp"));
	TEST_EXPECT(!reloaded.save(error) && error.code == "document.write" && reloaded.dirty());
	TEST_EXPECT(read_file_text(dir.file("items.def"), retained, message) && retained == external);
	return 0;
}
static int collections() {
	editor_test::TempProjectDir dir("opennova_catalog_collections_test");
	TEST_EXPECT(editor_test::write_text(dir.file("weapon.def"), "weapon \"WPN_ONE\"\nend\n"));
	EditableDocument document; Diagnostic error;
	TEST_EXPECT(document.load(dir.file("weapon.def"), "weapon.def", AssetKind::WeaponDefs, "jo", error));
	const auto parent = document.rows()[0]->id;
	CatalogEdit edit; edit.operation = CatalogOperation::Add; edit.address = {parent, DefRecordKind::Action, 0};
	TEST_EXPECT(document.apply(edit, error)); const auto child = document.last_added();
	const CatalogAddress action{parent, DefRecordKind::Action, child};
	TEST_EXPECT(document.apply(field(action, "name", std::string("Fire")), error));
	TEST_EXPECT(document.apply(field(action, "function", std::string("Shoot")), error));
	TEST_EXPECT(document.apply(field(action, "function_args_count", int64_t(2)), error));
	TEST_EXPECT(document.apply(field(action, "function_args[0]", int64_t(8)), error));
	TEST_EXPECT(document.apply(field(action, "ctrl_register", std::string("trigger")), error));
	TEST_EXPECT(document.apply(field(action, "ctrl_increment", int64_t(1)), error));
	TEST_EXPECT(document.apply(field(action, "duplicate_sound_delay", int64_t(3)), error));
	TEST_EXPECT(document.apply(field({parent, DefRecordKind::Weapon, 0}, "weaponweight", 1.25), error));
	TEST_EXPECT(std::get<DefWeaponDef>(document.rows()[0]->data).weaponweight_fp16 == 81920);
    TEST_EXPECT(!document.apply(field(action, "function_args_count", int64_t(5)), error));
    TEST_EXPECT(document.apply(field(action, "function_args_count", int64_t(1)), error));
    TEST_EXPECT(static_cast<const DefWeaponAction *>(document.record(action))->function_args[1] == 0);
    document.undo();
    TEST_EXPECT(document.save(error));
    edit.operation = CatalogOperation::Duplicate; edit.address = action;
	TEST_EXPECT(document.apply(edit, error)); const auto duplicate = document.last_added();
	TEST_EXPECT(duplicate != child);
	edit.operation = CatalogOperation::Remove; edit.address.child = child;
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
	session.handle(make_request(EditorRequestKind::CreateMissing));
    session.handle(make_request(EditorRequestKind::CreateCatalog, "ammo.def"));
    TEST_EXPECT(session.document_for("ammo.def"));
    session.handle(make_request(EditorRequestKind::CreateCatalog, "ammo.def"));
    TEST_EXPECT(session.document_for("ammo.def")->rows().size() == 1);
	session.handle(make_request(EditorRequestKind::OpenDocument, "items.def"));
	auto *document = session.document_for(); TEST_EXPECT(document);
	const auto id = document->rows()[0]->id;
	auto request = make_request(EditorRequestKind::EditRecord);
	request.catalog_edit = field({id, DefRecordKind::Item, 0}, "graphic", std::string("missing"));
	session.handle(request);
	session.handle(make_request(EditorRequestKind::Play));
	TEST_EXPECT(platform.spawns == 0 && !session.build_running());
	TEST_EXPECT(session.view().diagnostics.back().code == "build.unsaved");
	session.handle(make_request(EditorRequestKind::SaveAll));
	TEST_EXPECT(!document->dirty()); // semantic errors do not prevent saving
	session.handle(make_request(EditorRequestKind::Build)); session.finish_build();
	TEST_EXPECT(!session.view().last_build.ok);
	request.catalog_edit = field({id, DefRecordKind::Item, 0}, "graphic", std::string(""));
	session.handle(request);
	session.handle(make_request(EditorRequestKind::CloseProject));
	TEST_EXPECT(session.project_open() && session.view().unsaved_prompt);
	auto answer = make_request(EditorRequestKind::ResolveUnsaved); answer.unsaved_choice = UnsavedChoice::Cancel;
	session.handle(answer); TEST_EXPECT(session.project_open() && !session.view().unsaved_prompt);
	session.handle(make_request(EditorRequestKind::CloseProject));
	answer.unsaved_choice = UnsavedChoice::SaveAll;
	session.handle(answer); TEST_EXPECT(!session.project_open());
	session.handle(make_request(EditorRequestKind::OpenProject, dir.file("project")));
	session.handle(make_request(EditorRequestKind::Build)); session.finish_build();
	TEST_EXPECT(session.view().last_build.ok);
	session.handle(make_request(EditorRequestKind::OpenDocument, "items.def"));
	document = session.document_for(); TEST_EXPECT(document && !document->dirty());
	return 0;
}
static int malformed() {
	editor_test::TempProjectDir dir("opennova_catalog_malformed_test");
	TEST_EXPECT(editor_test::write_text(dir.file("items.def"), "begin \"Bad\"\nhp twelve\nend\n"));
	EditableDocument document; Diagnostic error;
	TEST_EXPECT(document.load(dir.file("items.def"), "items.def", AssetKind::ItemDefs, "jo", error));
	TEST_EXPECT(document.blocked() && document.parse_issues().front().line == 2);
	TEST_EXPECT(!document.save(error) && error.code == "document.unserializable");
	TEST_EXPECT(editor_test::write_text(dir.file("items.def"), "begin \"Open\"\nid 100001\n"));
	TEST_EXPECT(document.load(dir.file("items.def"), "items.def", AssetKind::ItemDefs, "jo", error));
	TEST_EXPECT(document.blocked());
	return 0;
}
static int remove_last_item() {
    editor_test::TempProjectDir dir("opennova_catalog_empty_test");
    TEST_EXPECT(editor_test::write_text(dir.file("items.def"), "begin \"One\"\nid 100001\ntype marker\npcvehicle_spawnlist 8\nend\n"));
    EditableDocument document; Diagnostic error;
    TEST_EXPECT(document.load(dir.file("items.def"), "items.def", AssetKind::ItemDefs, "jo", error));
    CatalogEdit remove; remove.operation = CatalogOperation::Remove;
    remove.address = {document.rows()[0]->id, DefRecordKind::Item, 0};
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
int main() { return history_and_save() || collections() || session_gate() || malformed() || remove_last_item(); }
