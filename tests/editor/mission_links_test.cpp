// A mission's logic in its views (ADR 0046 S24, In the views), over the minted synth_logic mission. S0: the
// pick a board chip asks of the views (the workspace's document `pick`, session/workspace_parts): a trigger's
// parameter slot set, reported by the record's identity and its slot, refused for a record that is no trigger
// or action and for a slot past 3 or left out, and ended by {}.
#include <cstdio>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/documents/mission_document.h>
#include <editor/documents/mission_table.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/workspace_parts.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
using opennova::io::JsonValue;

namespace {

// The mission's entry in the workspace's documents, as the view reports it.
JsonValue document_entry(const SessionView &view, const std::string &path) {
	const JsonValue workspace = workspace_to_json(view);
	if (const JsonValue *documents = workspace.get("documents"))
		for (const JsonValue &entry : documents->array)
			if (entry.get_string("path", "") == path) return entry;
	return JsonValue();
}

int test_pick() {
	editor_test::TempProjectDir dir("opennova_mission_links_pick");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Links"));
	editor_test::create_missing_files(session);
	const std::string repo = test_paths_repo_root(__FILE__);
	TEST_EXPECT(editor_test::write_bytes(session.view().project.root + "/missions/logic.bms",
	                                     test_io::read_file(repo + "/fixtures/bms/synth_logic.bms")));
	editor_test::handle_to_end(session, request::rescan());
	editor_test::handle_to_end(session, request::open_document("missions/logic.bms"));
	const auto *mission = dynamic_cast<const MissionDocument *>(session.document_for("missions/logic.bms"));
	TEST_EXPECT(mission);
	if (!mission) return 1;
	const std::string path = mission->path();
	const Node *event = mission->rows_of(MissionKind::Event)[0];
	const std::vector<Document::Collection> lists = mission->collections_of({event->id, event->kind, 0});
	TEST_EXPECT(!lists.empty() && !lists[0].ids.empty());
	const NodeId trigger = lists[0].ids[0];
	const Node *organic = mission->rows_of(MissionKind::Organic)[0];
	const auto set = [&](const std::string &pick) {
		const std::string change = "{\"document\": {\"path\": \"" + path + "\", \"pick\": " + pick + "}}";
		std::vector<WorkspaceRefusal> refusals;
		SessionView &view = const_cast<SessionView &>(session.view());
		apply_workspace_change(view, change, refusals);
		return refusals;
	};
	// No pick at first.
	JsonValue entry = document_entry(session.view(), path);
	TEST_EXPECT(entry.get("pick") && entry.get("pick")->is_null());
	// The trigger's param2 asked for.
	TEST_EXPECT(set("{\"record\": " + std::to_string(trigger) + ", \"slot\": 1}").empty());
	const WorkspaceView::Pick &held = session.view().workspace.document(path).pick;
	TEST_EXPECT(held.active() && held.record.child == trigger && held.slot == 1);
	entry = document_entry(session.view(), path);
	const JsonValue *pick = entry.get("pick");
	TEST_EXPECT(pick && pick->get_number("record", 0) == double(trigger) && pick->get_number("slot", -1) == 1);
	// Refused, the pick kept: an entity, a slot past 3.
	TEST_EXPECT(set("{\"record\": " + std::to_string(organic->id) + ", \"slot\": 0}").size() == 1);
	TEST_EXPECT(set("{\"record\": " + std::to_string(trigger) + ", \"slot\": 4}").size() == 1);
	TEST_EXPECT(set("{\"record\": " + std::to_string(trigger) + "}").size() == 1);
	TEST_EXPECT(session.view().workspace.document(path).pick.slot == 1);
	// Ended.
	TEST_EXPECT(set("{}").empty());
	TEST_EXPECT(!session.view().workspace.document(path).pick.active());
	entry = document_entry(session.view(), path);
	TEST_EXPECT(entry.get("pick") && entry.get("pick")->is_null());
	return 0;
}

} // namespace

int main() {
	if (test_pick() != 0) return 1;
	std::printf("editor_mission_links: all tests passed\n");
	return 0;
}
