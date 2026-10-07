// The jumps a keystroke or a right-click makes (ADR 0046 DI-18, editor/graph/jump_queries), over a real session
// holding the fixture's item catalog, the minted mission and the synth models its items draw. A search in one
// scope: the files alone or the names alone, ranked by how each holds the text (the name itself, then a name the
// text starts, then one holding it, then one found by its words or a record naming it), every file and name as
// the graph lists them in the scope all. Find usages: of a file, of a record by its locator (or its path, as a
// finding names it), none for a record defining no name; its subject in words. Go to definition from a record:
// what its first field naming something that resolves names (a mission entity's item, an item's model). On the
// wire: the project_search query's scope and the usages query's locator, each refused where it does not read;
// the workspace's finder, its scope and Find usages' subject, its text afresh in another scope, a file renamed
// followed, a file gone closing it, the refusals; an item record's Place in mission, the definition viewport's
// place_in_mission command, arming the mission's Place tool with the item, refused for a record that is no item,
// for several rows and with no mission open.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/documents/mission_document.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/jump_queries.h>
#include <editor/graph/reference_queries.h>
#include <editor/preview/mission_viewport.h>
#include <editor/preview/model_placement.h>
#include <editor/preview/viewports.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/session/workspace_parts.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
using opennova::io::JsonValue;

namespace {

using editor_test::NoProcess;
using FindScope = WorkspaceView::FindScope;

constexpr const char *kMission = "missions/synth_logic.bms";

struct Rig {
	editor_test::TempProjectDir dir;
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	std::string items, weapons;

	explicit Rig(const char *name) : dir(name) {}

	bool open(bool open_mission = true) {
		session.handle(request::new_project(dir.file("project"), "Jumps"));
		session.run_operations();
		editor_test::create_missing_files(session);
		const SessionView &v = session.view();
		const AssetEntry *catalog = v.project.scan ? v.project.scan->find("items.def") : nullptr;
		const AssetEntry *weapon = v.project.scan ? v.project.scan->find("weapon.def") : nullptr;
		if (!catalog || !weapon) return false;
		items = catalog->relative_path;
		weapons = weapon->relative_path;
		const std::string repo = test_paths_repo_root(__FILE__);
		if (!editor_test::write_bytes(v.project.root + "/" + items, test_io::read_file(repo + "/fixtures/def/items.def")) ||
		    !editor_test::write_text(v.project.root + "/" + weapons, "weapon \"Searchgun\"\nend\n") ||
		    !editor_test::write_bytes(v.project.root + "/" + kMission, test_io::read_file(repo + "/fixtures/bms/synth_logic.bms")) ||
		    !editor_test::write_bytes(v.project.root + "/missions/synth_logic.bin",
		                              test_io::read_file(repo + "/fixtures/bms/synth_logic.bin")))
			return false;
		for (const char *model : {"pump", "armory", "shed"})
			if (!editor_test::write_bytes(v.project.root + "/models/" + model + ".3di",
			                              test_io::read_file(repo + "/fixtures/threedi/synth/" + model + ".3di")))
				return false;
		session.handle(request::rescan());
		session.run_operations();
		session.handle(request::open_document(items));
		if (open_mission) session.handle(request::open_document(kMission));
		session.poll();
		return session.document_for(items) && v.findings.graph;
	}
	const AssetGraph &graph() const { return *session.view().findings.graph; }
	const AssetScan &scan() const { return *session.view().project.scan; }
	NodeAddress record(const std::string &path, const char *name) {
		NodeAddress out;
		if (const Document *document = session.document_for(path)) find_definition(AssetGraph(), *document, name, out);
		return out;
	}
	std::string locator(const std::string &path, const NodeAddress &address) {
		const Document *document = session.document_for(path);
		return document ? document->locator(address) : std::string();
	}
	NodeAddress entity_of(int64_t item) {
		const auto *mission = dynamic_cast<const MissionDocument *>(session.document_for(kMission));
		if (!mission) return NodeAddress();
		for (const MissionKind kind : {MissionKind::Item, MissionKind::Building, MissionKind::Organic, MissionKind::Marker})
			for (const Node *row : mission->rows_of(kind)) {
				Value value;
				const NodeAddress address{row->id, row->kind, 0};
				if (mission->get(address, "item", value) && std::get_if<int64_t>(&value) && std::get<int64_t>(value) == item)
					return address;
			}
		return NodeAddress();
	}
	JsonValue query(const char *name, const std::string &args, std::string &error) {
		JsonValue parsed;
		std::string parse_error;
		opennova::io::json_parse(args, parsed, parse_error);
		error.clear();
		return session.query(name, parsed, error);
	}
};

std::string refusal(ProjectSession &session) {
	for (const Diagnostic &finding : session.outcome().findings) return finding.message;
	return std::string();
}

// The scopes: the files alone and the names alone, ranked; all as the graph lists them.
static int test_search_scopes() {
	Rig rig("opennova_editor_jumps_search");
	TEST_EXPECT(rig.open());
	if (rig.items.empty()) return 1;
	const std::vector<GraphSearchHit> all = search_project(rig.graph(), "pump", SearchScope::All);
	const std::vector<GraphSearchHit> graph = rig.graph().search("pump");
	TEST_EXPECT(all.size() == graph.size() && !all.empty());
	const std::vector<GraphSearchHit> files = search_project(rig.graph(), "pump", SearchScope::Files);
	const std::vector<GraphSearchHit> names = search_project(rig.graph(), "pump", SearchScope::Names);
	TEST_EXPECT(!files.empty() && std::all_of(files.begin(), files.end(), [](const GraphSearchHit &hit) { return !hit.symbol; }));
	TEST_EXPECT(!names.empty() && std::all_of(names.begin(), names.end(), [](const GraphSearchHit &hit) { return hit.symbol; }));
	TEST_EXPECT(files.size() + names.size() == all.size());
	TEST_EXPECT(files.front().file == "models/pump.3di");
	// The name itself first: a file's stem the text ("shed" before a name only holding it).
	const std::vector<GraphSearchHit> shed = search_project(rig.graph(), "SHED", SearchScope::Files);
	TEST_EXPECT(!shed.empty() && shed.front().file == "models/shed.3di");
	// A name found by its words after one whose own name holds the text: the rifleman by its catalog's name.
	const std::vector<GraphSearchHit> rifleman = search_project(rig.graph(), "rifleman", SearchScope::Names);
	TEST_EXPECT(!rifleman.empty() && rifleman.front().words.find("Rifleman") != std::string::npos);
	TEST_EXPECT(search_project(rig.graph(), "", SearchScope::Files).empty());
	SearchScope scope = SearchScope::All;
	TEST_EXPECT(search_scope_from_token("names", scope) && scope == SearchScope::Names && !search_scope_from_token("nope", scope));
	std::printf("test_search_scopes passed\n");
	return 0;
}

// Find usages: of a file, of a record by its locator or its path, none for a record defining no name; in words.
static int test_usages_at() {
	Rig rig("opennova_editor_jumps_usages");
	TEST_EXPECT(rig.open());
	if (rig.items.empty()) return 1;
	const std::vector<const GraphEdge *> model = usages_at(rig.graph(), "models/pump.3di", "");
	TEST_EXPECT(model.size() == rig.graph().usages_of("models/pump.3di").size() && !model.empty());
	const NodeAddress pump = rig.record(rig.items, "106100");
	const std::string at = rig.locator(rig.items, pump);
	const std::vector<const GraphEdge *> placed = usages_at(rig.graph(), rig.items, at);
	TEST_EXPECT(placed.size() == 3);
	TEST_EXPECT(std::all_of(placed.begin(), placed.end(), [](const GraphEdge *edge) { return edge->source == kMission; }));
	// The same record by its path, as a finding names it.
	const Document *items = rig.session.document_for(rig.items);
	TEST_EXPECT(items && usages_at(rig.graph(), rig.items, items->record_path(pump)).size() == 3);
	TEST_EXPECT(usages_at(rig.graph(), rig.items, "no such record").empty());
	TEST_EXPECT(usages_subject_words(rig.graph(), "models/pump.3di", "") == "pump.3di");
	const std::string words = usages_subject_words(rig.graph(), rig.items, at);
	TEST_EXPECT(words.find("106100") != std::string::npos && words.find("items.def") != std::string::npos);
	// A mission's entity: the events naming its SSN (and whatever else does), each in the mission.
	const NodeAddress walker = rig.entity_of(106102);
	const std::vector<const GraphEdge *> events = usages_at(rig.graph(), kMission, rig.locator(kMission, walker));
	TEST_EXPECT(std::all_of(events.begin(), events.end(), [](const GraphEdge *edge) { return edge->source == kMission; }));
	std::printf("test_usages_at passed\n");
	return 0;
}

// Go to definition from a record: an entity's item, an item's model; nothing for a record naming nothing.
static int test_record_definition() {
	Rig rig("opennova_editor_jumps_definition");
	TEST_EXPECT(rig.open());
	if (rig.items.empty()) return 1;
	const Document *mission = rig.session.document_for(kMission);
	const Document *items = rig.session.document_for(rig.items);
	TEST_EXPECT(mission && items);
	if (!mission || !items) return 1;
	std::vector<ReferenceTarget> targets;
	TEST_EXPECT(record_definition(rig.graph(), rig.scan(), *mission, rig.entity_of(106102), targets) && targets.size() == 1 &&
	            targets.front().file == rig.items &&
	            targets.front().locator == rig.locator(rig.items, rig.record(rig.items, "106102")));
	TEST_EXPECT(record_definition(rig.graph(), rig.scan(), *items, rig.record(rig.items, "106100"), targets) &&
	            targets.front().file == "models/pump.3di");
	TEST_EXPECT(!record_definition(rig.graph(), rig.scan(), *items, NodeAddress(), targets) && targets.empty());
	std::printf("test_record_definition passed\n");
	return 0;
}

// The wire: project_search's scope, usages' locator; the finder's workspace part.
static int test_wire() {
	Rig rig("opennova_editor_jumps_wire");
	TEST_EXPECT(rig.open());
	if (rig.items.empty()) return 1;
	std::string error;
	JsonValue answer = rig.query("project_search", R"({"text": "pump", "scope": "files"})", error);
	const JsonValue *hits = answer.get("hits");
	TEST_EXPECT(error.empty() && hits && hits->is_array() && !hits->array.empty() &&
	            hits->array.front().get_string("file", "") == "models/pump.3di");
	answer = rig.query("project_search", R"({"text": "pump", "scope": "names"})", error);
	hits = answer.get("hits");
	TEST_EXPECT(error.empty() && hits && !hits->array.empty() && hits->array.front().get_string("kind", "file") != "file");
	rig.query("project_search", R"({"text": "pump", "scope": "folders"})", error);
	TEST_EXPECT(error.find("no scope \"folders\"") != std::string::npos);
	const NodeAddress pump = rig.record(rig.items, "106100");
	const std::string at = rig.locator(rig.items, pump);
	answer = rig.query("usages", R"({"path": ")" + rig.items + R"(", "locator": ")" + at + R"("})", error);
	TEST_EXPECT(error.empty() && answer.get_number("count", 0) == 3);
	rig.query("usages", R"({"locator": ")" + at + R"("})", error);
	TEST_EXPECT(error.find("a locator names a record") != std::string::npos);

	// The finder: Go to file, then Find usages of the pump's record, its text afresh; refusals.
	const SessionView &v = rig.session.view();
	const WorkspaceView::ProjectFind &find = v.workspace.project_find;
	TEST_EXPECT(rig.session.handle(request::set_workspace(R"({"project_find": {"open": true, "scope": "files", "text": "pu"}})")) &&
	            find.open && find.scope == FindScope::Files && find.text == "pu" && find.path.empty());
	const uint64_t opened = v.workspace.opened;
	TEST_EXPECT(rig.session.handle(request::set_workspace(R"({"project_find": {"scope": "usages", "path": "items.def", "locator": ")" +
	                                                     at + R"("}})")) &&
	            find.scope == FindScope::Usages && find.path == rig.items && find.locator == at && find.text.empty() &&
	            v.workspace.opened == opened + 1);
	const JsonValue workspace = workspace_to_json(v);
	const JsonValue *part = workspace.get("project_find");
	TEST_EXPECT(part && part->get_string("scope", "") == "usages" && part->get_string("path", "") == rig.items &&
	            part->get_string("locator", "") == at);
	rig.session.handle(request::set_workspace(R"({"project_find": {"scope": "everything"}})"));
	TEST_EXPECT(refusal(rig.session).find("no scope \"everything\"") != std::string::npos && find.scope == FindScope::Usages);
	rig.session.handle(request::set_workspace(R"({"project_find": {"path": "gone.def"}})"));
	TEST_EXPECT(refusal(rig.session).find("No project file is gone.def") != std::string::npos && find.path == rig.items);
	// Another scope keeps no subject; closed and opened in Find usages with none, refused.
	TEST_EXPECT(rig.session.handle(request::set_workspace(R"({"project_find": {"scope": "names"}})")) && find.path.empty() &&
	            find.locator.empty());
	rig.session.handle(request::set_workspace(R"({"project_find": {"open": false}})"));
	rig.session.handle(request::set_workspace(R"({"project_find": {"open": true, "scope": "usages"}})"));
	TEST_EXPECT(refusal(rig.session).find("Find usages names a project file") != std::string::npos && !find.open);
	// A file renamed: Find usages of it follows it; the file gone: it closes.
	TEST_EXPECT(rig.session.handle(request::set_workspace(R"({"project_find": {"open": true, "scope": "usages", "path": "shed.3di"}})")) &&
	            find.open && find.path == "models/shed.3di");
	rig.session.handle(request::rename_asset("models/shed.3di", "hut.3di"));
	rig.session.run_operations();
	TEST_EXPECT(find.open && find.path == "models/hut.3di");
	std::error_code removed;
	TEST_EXPECT(std::filesystem::remove(v.project.root + "/models/hut.3di", removed));
	rig.session.handle(request::rescan());
	rig.session.run_operations();
	TEST_EXPECT(!find.open);
	std::printf("test_wire passed\n");
	return 0;
}

// An item record's Place in mission: the definition viewport's place_in_mission command.
static int test_place_item_in_mission() {
	Rig rig("opennova_editor_jumps_place");
	TEST_EXPECT(rig.open());
	if (rig.items.empty()) return 1;
	const SessionView &v = rig.session.view();
	const NodeAddress pump = rig.record(rig.items, "106100");
	const auto command = [&](const std::string &path, std::vector<NodeId> ids) {
		ViewportCommand place;
		place.name = "place_in_mission";
		place.kind = ViewportKind::Definition;
		place.ids = std::move(ids);
		rig.session.handle(request::edit_in_viewport(path, place));
	};
	const auto armed = [&](int64_t item) {
		const auto *viewport = static_cast<const MissionViewport *>(rig.session.viewports().find(kMission, ViewportKind::Mission));
		return viewport && viewport->options().tool == MissionTool::Place && viewport->options().item == item &&
		       v.documents.active == kMission;
	};
	rig.session.handle(request::open_document(rig.items));
	TEST_EXPECT(place_in_mission_target(v) == kMission);
	command(rig.items, {pump.row});
	TEST_EXPECT(rig.session.outcome().done() && armed(106100));
	TEST_EXPECT(v.activity.status.find("Wire Test Pump (106100)") != std::string::npos &&
	            v.activity.status.find("Place is armed in synth_logic.bms") != std::string::npos);
	// The record shown, with no ids: the selection's in the table.
	rig.session.handle(request::open_document(rig.items));
	const NodeAddress armory = rig.record(rig.items, "106101");
	rig.session.handle(request::select_record(rig.items, armory));
	rig.session.poll();
	command(rig.items, {});
	TEST_EXPECT(rig.session.outcome().done() && armed(106101));
	// Refused: two rows; a record that is no item (a weapon); no mission open.
	rig.session.handle(request::open_document(rig.items));
	command(rig.items, {pump.row, armory.row});
	TEST_EXPECT(rig.session.outcome().refused && refusal(rig.session).find("one item record") != std::string::npos);
	rig.session.handle(request::open_document(rig.weapons));
	const NodeAddress gun = rig.record(rig.weapons, "Searchgun");
	command(rig.weapons, {gun.row});
	TEST_EXPECT(rig.session.outcome().refused && refusal(rig.session).find("no item record") != std::string::npos);
	rig.session.handle(request::close_document(kMission));
	command(rig.items, {pump.row});
	TEST_EXPECT(rig.session.outcome().refused && refusal(rig.session).find("No mission is open") != std::string::npos);
	std::printf("test_place_item_in_mission passed\n");
	return 0;
}

} // namespace

int main() {
	int failed = 0;
	failed += test_search_scopes();
	failed += test_usages_at();
	failed += test_record_definition();
	failed += test_wire();
	failed += test_place_item_in_mission();
	if (failed) std::printf("%d test(s) failed\n", failed);
	return failed ? 1 : 0;
}
