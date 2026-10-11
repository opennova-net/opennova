// The Event board's portable half (ADR 0046 S24, The board), over the minted synth_logic mission. The
// mission_logic query answers by its op's row (session/mission_queries.h): every op its catalog offers is one
// of the table's tokens, each answered by its own row (none falls through as unknown), the moved answers as
// before (an event's form); and (a SKIP-LEG without OPENNOVA_JO_DIR) every shipped event given the form a
// card reads, its lists as the event holds them.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <base/vfs/vfs.h>
#include <base/vfs/vfs_decode.h>
#include <editor/documents/document_types.h>
#include <editor/documents/mission_document.h>
#include <editor/documents/mission_logic.h>
#include <editor/documents/mission_sentence.h>
#include <editor/documents/mission_table.h>
#include <editor/session/editor_queries.h>
#include <editor/session/mission_queries.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
using opennova::io::JsonValue;

namespace {

bool same(const std::string &got, const std::string &want) {
	if (got != want) std::printf("  got:  %s\n  want: %s\n", got.c_str(), want.c_str());
	return got == want;
}

std::unique_ptr<Document> open(const std::vector<uint8_t> &bytes, const char *name) {
	const DocumentType *type = document_type_for(AssetKind::Mission);
	if (!type) return nullptr;
	std::unique_ptr<Document> document = records_of(type->make());
	Diagnostic error;
	if (!document || !document->load_bytes(bytes, name, AssetKind::Mission, "jo", error)) return nullptr;
	return document;
}

// The op table (S24 S0): the query's op param takes exactly the table's tokens, each answered by its own row.
int test_op_table() {
	const EditorQueryRow &row = editor_query_row(EditorQueryKind::MissionLogic);
	std::vector<std::string> offered;
	for (size_t c = 0; c < row.choice_count; ++c)
		if (std::string(row.choices[c].param) == "op")
			for (size_t i = 0; const char *token = row.choices[c].token(i); ++i) offered.push_back(token);
	std::vector<std::string> table;
	for (size_t i = 0; const char *token = mission_logic_op_choice(i); ++i) table.push_back(token);
	TEST_EXPECT(!table.empty() && offered == table);
	for (const char *op : {"form", "types", "add", "retype", "move", "negate", "join"})
		TEST_EXPECT(std::find(table.begin(), table.end(), op) != table.end());

	editor_test::TempProjectDir dir("opennova_mission_board_ops");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Board"));
	editor_test::create_missing_files(session);
	const std::string repo = test_paths_repo_root(__FILE__);
	TEST_EXPECT(editor_test::write_bytes(session.view().project.root + "/missions/logic.bms",
	                                     test_io::read_file(repo + "/fixtures/bms/synth_logic.bms")));
	editor_test::handle_to_end(session, request::rescan());
	editor_test::handle_to_end(session, request::open_document("missions/logic.bms"));
	const auto *mission = dynamic_cast<const MissionDocument *>(session.document_for("missions/logic.bms"));
	TEST_EXPECT(mission);
	if (!mission) return 1;
	const Node *event = mission->rows_of(MissionKind::Event)[1];
	const auto ask = [&](const std::string &args, std::string &error) {
		JsonValue parsed;
		opennova::io::json_parse(args, parsed, error);
		return session.query("mission_logic", parsed, error);
	};
	// Each op reaches its row: what it refuses is its own need, never an unknown op.
	for (const std::string &op : table) {
		std::string error;
		ask("{\"path\": \"missions/logic.bms\", \"op\": \"" + op + "\"}", error);
		TEST_EXPECT(error.find("no op") == std::string::npos);
	}
	std::string error;
	const JsonValue form = ask("{\"path\": \"missions/logic.bms\", \"op\": \"form\", \"id\": " + std::to_string(event->id) + "}", error);
	TEST_EXPECT(error.empty() && same(form.get_string("sentence", ""), "When event 1 has fired, then, after 5.1 s, kill group 2."));
	// An op the table has none of is refused by the param's choices, naming the ops.
	const JsonValue unknown = ask("{\"path\": \"missions/logic.bms\", \"op\": \"fold\"}", error);
	TEST_EXPECT(unknown.is_null() && error.find("form") != std::string::npos);
	return 0;
}

// Every shipped event given the form its card reads: its lists as the event holds them, its sentence.
int test_retail() {
	const std::string root = retail::install();
	if (root.empty()) return retail::skip_leg("OPENNOVA_JO_DIR (every shipped event's form)");
	std::vector<std::string> expansions = opennova::vfs_list_expansions(root);
	expansions.insert(expansions.begin(), std::string());
	std::set<std::string> seen;
	size_t missions = 0, events = 0;
	for (const std::string &expansion : expansions) {
		opennova::Vfs game;
		game.set_scr_policy(opennova::VFS_SCR_FORCE_JO_DFX2);
		TEST_EXPECT(game.mount_game(root, expansion, opennova::VfsMountMode::Packed));
		for (const opennova::VfsFileLocation &file : game.list_files()) {
			if (retail::lower_ascii(std::filesystem::path(file.logical_name).extension().string()) != ".bms") continue;
			if (!seen.insert(file.source_path + "|" + retail::lower_ascii(file.logical_name)).second) continue;
			std::vector<uint8_t> bytes;
			TEST_EXPECT(game.read_file(file.logical_name, bytes));
			std::unique_ptr<Document> document = open(bytes, file.logical_name.c_str());
			TEST_EXPECT(document && !document->blocked());
			if (!document) continue;
			++missions;
			const auto &m = dynamic_cast<const MissionDocument &>(*document);
			const DocumentMissionNames names(m);
			for (const Node *row : m.rows_of(MissionKind::Event)) {
				LogicEventForm form;
				TEST_EXPECT(logic_event_form(m, row->id, names, form));
				const std::vector<Document::Collection> lists = m.collections_of({row->id, row->kind, 0});
				TEST_EXPECT(lists.size() == 2 && form.triggers == lists[0].ids.size() && form.actions == lists[1].ids.size());
				TEST_EXPECT(!form.words.sentence.empty());
				++events;
			}
		}
	}
	if (missions == 0) return retail::skip_leg("OPENNOVA_JO_DIR with the game's missions in its archives");
	std::printf("retail: %zu missions, %zu events given their form\n", missions, events);
	TEST_EXPECT(missions == 115);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	if (test_op_table() != 0) return 1;
	return test_retail();
}
