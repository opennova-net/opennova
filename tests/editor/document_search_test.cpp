// S12 D8 (ADR 0046 S12): find in a document and in the project. Over a project with one file of
// every document type (the def catalogs, a string table, a menu, a stylesheet, a model, a clip and
// an animation table), each document's fields are found as the Inspector shows them: a value
// searched finds its own record and field, any case unless the case must match; a def's number
// in the units its line writes (a turn rate in degrees a second, not the stored binary angle), a
// choice by its name. In the project, the files and the symbols whose names hold a text, each with
// its usages: a weapon's name with the item naming it, a symbol no lookup finds with none.
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include <editor/graph/asset_graph.h>
#include <editor/graph/reference_queries.h>
#include <editor/model/document_search.h>
#include <editor/model/field_text.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/anim_test_support.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;

namespace {

using editor_test::NoProcess;

// A new project's files, the item and weapon tables replaced, a model, and a clip and its table
// imported from the Blender add-on's scene text.
bool make_project(ProjectSession &session, const editor_test::TempProjectDir &dir) {
	session.handle(request::new_project(dir.file("project"), "Search"));
	editor_test::create_missing_files(session);
	const SessionView &v = session.view();
	const AssetEntry *items = v.project.scan->find("items.def");
	const AssetEntry *weapons = v.project.scan->find("weapon.def");
	if (!items || !weapons) return false;
	const std::string repo = test_paths_repo_root(__FILE__);
	const std::string source = dir.file("source");
	if (!editor_test::write_text(v.project.root + "/" + items->relative_path,
	                             "begin \"Searched Thing\"\nid 100300\ntype vehicle\nturn_rate 90\nprimary_weapon Searchgun\n"
	                             "end\n") ||
	    !editor_test::write_text(v.project.root + "/" + weapons->relative_path, "weapon \"Searchgun\"\nend\n") ||
	    !editor_test::write_bytes(v.project.root + "/models/armory.3di",
	                              test_io::read_file(repo + "/fixtures/threedi/synth/armory.3di")) ||
	    !editor_test::write_bytes(source + "/skinned.o3d", test_io::read_file(repo + "/fixtures/threedi/o3d/skinned.o3d")) ||
	    !editor_test::write_text(source + "/skin.o3a", editor_test::kSkinClips))
		return false;
	EditorRequest import = request::of(EditorRequestKind::ImportFiles);
	import.imports = {{source + "/skinned.o3d", {}}, {source + "/skin.o3a", {}}};
	session.handle(import);
	session.handle(request::rescan());
	return v.project.scan->find("SKIN.adm") && v.project.scan->find("walk.bad") &&
			v.project.scan->find("armory.3di");
}

std::string upper(std::string text) {
	for (char &c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	return text;
}

bool has_hit(const std::vector<DocumentHit> &hits, const NodeAddress &address, const std::string &field) {
	return std::any_of(hits.begin(), hits.end(),
	                   [&](const DocumentHit &hit) { return hit.address == address && hit.field == field; });
}

// A document's first field whose shown value has a few characters, found by that value: its own
// record and field among the hits, with the text as shown, the locator finding the record again;
// its upper case too, unless the case must match.
int check_self_search(const Document &document) {
	NodeAddress record;
	FieldSchema field;
	std::string text;
	const auto first = [&](const NodeAddress &address) {
		for (const FieldSchema &schema : document.fields(address.kind)) {
			const FieldUse applied = document.field_on(address, schema);
			Value value;
			if (schema.optional && !document.present(address, schema.id)) continue;
			if (applied.applies == Applicability::Ignored || !document.get(address, schema.id, value)) continue;
			// As the find shows it: a record's own choice by its name (Document::choices_on).
			std::vector<FieldChoice> offered;
			const std::string shown = field_text(schema, document.choices_on(address, applied, offered), value);
			if (shown.size() < 3 || shown.find('\n') != std::string::npos) continue;
			record = address;
			field = schema;
			text = shown;
			return true;
		}
		return false;
	};
	for (const auto &row : document.rows()) {
		if (first({row->id, row->kind, 0})) break;
		bool found = false;
		document.walk_records(*row, [&](const NodeAddress &nested, const Document::Placement &) {
			found = first(nested);
			return !found;
		});
		if (found) break;
	}
	TEST_EXPECT(!text.empty());
	if (text.empty()) return 1;
	const std::vector<DocumentHit> hits = find_in_document(document, text);
	const auto own = std::find_if(hits.begin(), hits.end(), [&](const DocumentHit &hit) {
		return hit.address == record && hit.field == field.id;
	});
	TEST_EXPECT(own != hits.end());
	if (own == hits.end()) return 1;
	TEST_EXPECT(own->text == text && own->at == 0 && own->label == field_title(field) &&
	            document.address_at(own->locator) == record && own->record == document.record_path(record));
	TEST_EXPECT(has_hit(find_in_document(document, upper(text)), record, field.id));
	SearchOptions exact;
	exact.match_case = true;
	if (upper(text) != text) TEST_EXPECT(!has_hit(find_in_document(document, upper(text), exact), record, field.id));
	TEST_EXPECT(has_hit(find_in_document(document, text, exact), record, field.id));
	return 0;
}

} // namespace

static int test_each_document_type() {
	editor_test::TempProjectDir dir("opennova_document_search");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	TEST_EXPECT(make_project(session, dir));
	const SessionView &view = session.view();
	for (const char *name : {"items.def", "weapon.def", "gametext.bin", "main.mnu", "menu_style.mns", "armory.3di",
	                         "walk.bad", "SKIN.adm"}) {
		const AssetEntry *entry = view.project.scan->find(name);
		TEST_EXPECT(entry != nullptr);
		if (!entry) continue;
		session.handle(request::open_document(entry->relative_path));
		const Document *document = session.document_for(entry->relative_path);
		TEST_EXPECT(document != nullptr);
		if (!document) continue;
		// Every document type's own value found where it is; a failure names the file.
		if (check_self_search(*document)) {
			std::printf("  (in %s)\n", name);
			return 1;
		}
		TEST_EXPECT(find_in_document(*document, std::string()).empty());
	}
	// A def's number is found as its line writes it: the turn rate in degrees a second (90), not
	// the binary angle it is stored as (90 x 192426); a choice by its name.
	const AssetEntry *items_entry = view.project.scan->find("items.def");
	const Document *items = items_entry ? session.document_for(items_entry->relative_path) : nullptr;
	TEST_EXPECT(items != nullptr);
	if (!items) return 1;
	NodeAddress item;
	TEST_EXPECT(find_definition(AssetGraph(), *items, "100300", item));
	const std::vector<DocumentHit> ninety = find_in_document(*items, "90");
	const auto turn = std::find_if(ninety.begin(), ninety.end(), [](const DocumentHit &hit) { return hit.field == "turn_rate"; });
	TEST_EXPECT(turn != ninety.end() && turn->text == "90" && turn->address == item);
	TEST_EXPECT(!has_hit(find_in_document(*items, std::to_string(90 * 192426)), item, "turn_rate"));
	TEST_EXPECT(has_hit(find_in_document(*items, "VEHICLE"), item, "type"));
	return 0;
}

static int test_project_search() {
	editor_test::TempProjectDir dir("opennova_project_search");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	TEST_EXPECT(make_project(session, dir));
	const SessionView &view = session.view();
	TEST_EXPECT(view.findings.graph != nullptr);
	if (!view.findings.graph) return 1;
	const AssetGraph &graph = *view.findings.graph;
	TEST_EXPECT(graph.search(std::string()).empty());
	// A weapon's name: the item that names it is its one use, found where it is used.
	const std::vector<GraphSearchHit> guns = graph.search("searchg");
	const auto gun = std::find_if(guns.begin(), guns.end(), [](const GraphSearchHit &hit) {
		return hit.symbol && hit.symbol->kind == ReferenceKind::Weapon && hit.name == "Searchgun";
	});
	TEST_EXPECT(gun != guns.end() && gun->usages == 1);
	if (gun != guns.end()) {
		const std::vector<const GraphEdge *> users = graph.users_of(*gun->symbol);
		TEST_EXPECT(users.size() == 1 && users[0]->field == "primary_weapon" &&
		            users[0]->source == view.project.scan->find("items.def")->relative_path);
	}
	// Files by name, before the symbols: the item table, with the usages of what it defines.
	const std::vector<GraphSearchHit> tables = graph.search("ITEMS");
	TEST_EXPECT(!tables.empty() && !tables.front().symbol && tables.front().name == "items.def");
	// Every symbol found is counted by the uses that reach exactly it: none for one no lookup finds.
	for (const GraphSearchHit &hit : graph.search("e"))
		if (hit.symbol) TEST_EXPECT(hit.usages == graph.users_of(*hit.symbol).size() && (!hit.symbol->inert || hit.usages == 0));
		else TEST_EXPECT(hit.usages == graph.usages_of(hit.file).size());
	return 0;
}

int main() {
	int failures = 0;
	failures += test_each_document_type();
	failures += test_project_search();
	if (failures == 0) std::printf("editor_document_search: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
