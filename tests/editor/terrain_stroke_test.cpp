// The terrain tools' shared seams (ADR 0046 S25): the undo across documents over the history's step marks (each
// step a process-wide serial, its gesture its link), an Undo in a view taking with its step the top step of every
// other document carrying the same link and a Redo making them again, an EndEdit by a gesture's token ending it in
// every document its batches changed and its cancel dropping them, Keep my edits in a view writing over its own
// file's outside change alone (a linked file's conflict refused and named), the terrain_stroke request on the wire
// (read back as written) and its refusals while no plane's planner is in, and the workspace's terrain brush and Terrain
// pane as set_workspace sets them.
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <base/io/file_io.h>
#include <base/io/json.h>
#include <editor/model/text_document.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/session_json.h>
#include <editor/session/view/session_view.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
using opennova::io::JsonValue;

namespace {

std::vector<uint8_t> bytes_of(const std::string &text) { return std::vector<uint8_t>(text.begin(), text.end()); }

// A span edit of a text: `length` characters at (line, column) replaced by `with`, under `gesture`.
Edit span(size_t line, size_t column, size_t length, std::string with, uint64_t gesture) {
	TextSpan at;
	at.line = line;
	at.column = column;
	at.length = length;
	Edit edit;
	edit.operation = EditOperation::Apply;
	edit.payload = std::make_shared<TextSpanEdit>(at, std::move(with));
	edit.gesture = gesture;
	return edit;
}

using editor_test::contains;

// The history's marks: a fresh serial for each step and each fold, the gesture as the link, the redo's the step
// undone; a cancel drops the open group's step.
int test_marks() {
	TextDocument document;
	Diagnostic error;
	TEST_EXPECT(document.load_bytes(bytes_of("abc\r\n"), "t.wac", AssetKind::Script, "jo", error));
	TEST_EXPECT(document.undo_mark() == StepMark() && document.redo_mark() == StepMark());
	TEST_EXPECT(document.apply({span(1, 1, 1, "x", 41)}, error));
	const StepMark first = document.undo_mark();
	TEST_EXPECT(first.serial != 0 && first.link == 41);
	TEST_EXPECT(document.apply({span(1, 2, 1, "y", 41)}, error)); // folds: a fresh serial, the same link
	const StepMark folded = document.undo_mark();
	TEST_EXPECT(folded.serial > first.serial && folded.link == 41);
	document.end_edit_group();
	TEST_EXPECT(document.apply({span(1, 3, 1, "z", 0)}, error));
	const StepMark plain = document.undo_mark();
	TEST_EXPECT(plain.serial > folded.serial && plain.link == 0);
	document.undo();
	TEST_EXPECT(document.redo_mark() == plain && document.undo_mark() == folded);
	// A cancel of a gesture whose group is open drops its step (no redo left of it); one not open does nothing.
	document.redo();
	TEST_EXPECT(!document.cancel_gesture(41));
	TEST_EXPECT(document.apply({span(1, 1, 3, "qqq", 42)}, error) && document.text() == "qqq\r\n");
	TEST_EXPECT(document.cancel_gesture(42) && document.text() == "xyz\r\n" && !document.can_redo());
	std::printf("marks: serials per step and fold, links, a cancel dropping the open group\n");
	return 0;
}

// Two scripts edited under one gesture: an Undo in either takes both, a Redo makes both again; an EndEdit by the
// token ends it in both, and its cancel drops both.
int test_linked_undo() {
	test_temp::TempDir dir("opennova_editor_terrain_stroke");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Linked"));
	const std::string root = session.view().project.root;
	TEST_EXPECT(test_io::write_text(root + "/scripts/a.wac", "aaa\r\n"));
	TEST_EXPECT(test_io::write_text(root + "/scripts/b.wac", "bbb\r\n"));
	editor_test::handle_to_end(session, request::rescan());
	const SessionView &v = session.view();
	const AssetEntry *a_entry = v.project.scan->find("a.wac");
	const AssetEntry *b_entry = v.project.scan->find("b.wac");
	TEST_EXPECT(a_entry && b_entry);
	const std::string a = a_entry->relative_path, b = b_entry->relative_path;
	session.handle(request::open_document(a));
	session.handle(request::open_document(b));
	const auto text = [&](const std::string &path) {
		for (const auto &document : v.documents.open)
			if (document->path() == path) return text_of(*document)->text();
		return std::string();
	};
	const uint64_t gesture = next_edit_gesture();
	session.handle(request::edit_record(a, span(1, 1, 3, "AAA", gesture)));
	session.handle(request::edit_record(b, span(1, 1, 3, "BBB", gesture)));
	session.handle(request::end_edit(std::string(), gesture));
	TEST_EXPECT(v.documents.gestures.empty() && text(a) == "AAA\r\n" && text(b) == "BBB\r\n");
	// The view's next step names its link's other document; an Undo in a takes b's with it.
	session.handle(request::open_document(a));
	TEST_EXPECT(v.documents.undo_next.path == b || v.documents.undo_next.path == a);
	TEST_EXPECT(v.documents.undo_next.linked.size() == 1);
	session.handle(request::undo(a));
	TEST_EXPECT(session.outcome().done() && session.outcome().stepped.size() == 2);
	TEST_EXPECT(text(a) == "aaa\r\n" && text(b) == "bbb\r\n");
	TEST_EXPECT(contains(v.activity.status, "Undid: "));
	session.handle(request::redo(a));
	TEST_EXPECT(session.outcome().stepped.size() == 2 && text(a) == "AAA\r\n" && text(b) == "BBB\r\n");
	// b edited since: an Undo in a takes a's alone and names b as keeping its later edit.
	session.handle(request::edit_record(b, span(1, 1, 1, "c", 0)));
	session.handle(request::undo(a));
	TEST_EXPECT(text(a) == "aaa\r\n" && text(b) == "cBB\r\n");
	TEST_EXPECT(contains(v.activity.status, "b.wac was edited since"));
	session.handle(request::undo(b));
	session.handle(request::undo(b));
	TEST_EXPECT(text(b) == "bbb\r\n");
	// A gesture open in both cancelled by its token: both as it found them, no redo of it.
	const uint64_t cancelled = next_edit_gesture();
	session.handle(request::edit_record(a, span(1, 1, 1, "X", cancelled)));
	session.handle(request::edit_record(b, span(1, 1, 1, "Y", cancelled)));
	TEST_EXPECT(text(a) == "Xaa\r\n" && text(b) == "Ybb\r\n" && v.documents.gestures.size() == 2);
	session.handle(request::end_edit(std::string(), cancelled, true));
	TEST_EXPECT(text(a) == "aaa\r\n" && text(b) == "bbb\r\n" && v.documents.gestures.empty());
	std::printf("linked undo: one gesture's steps in two documents undone, redone and cancelled together\n");
	return 0;
}

// Keep my edits on a view's file (its conflict's fix, save_over) writes over that file's outside change alone: a file
// its Save writes beside it (one gesture's link) that changed outside the editor too is refused, named in the status,
// its own conflict deciding it; nothing the other program saved there is lost unasked.
int test_scope_save_over() {
	test_temp::TempDir dir("opennova_editor_terrain_save_over");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Over"));
	const std::string root = session.view().project.root;
	TEST_EXPECT(test_io::write_text(root + "/scripts/a.wac", "aaa\r\n"));
	TEST_EXPECT(test_io::write_text(root + "/scripts/b.wac", "bbb\r\n"));
	editor_test::handle_to_end(session, request::rescan());
	const SessionView &v = session.view();
	const std::string a = v.project.scan->find("a.wac")->relative_path, b = v.project.scan->find("b.wac")->relative_path;
	session.handle(request::open_document(a));
	session.handle(request::open_document(b));
	const uint64_t gesture = next_edit_gesture();
	session.handle(request::edit_record(a, span(1, 1, 3, "AAA", gesture)));
	session.handle(request::edit_record(b, span(1, 1, 3, "BBB", gesture)));
	session.handle(request::end_edit(std::string(), gesture));
	// Another program saves over both.
	TEST_EXPECT(test_io::write_text(root + "/" + a, "outside a, longer\r\n"));
	TEST_EXPECT(test_io::write_text(root + "/" + b, "outside b, longer\r\n"));
	session.handle(request::open_document(a));
	editor_test::handle_to_end(session, request::save_over(a));
	std::string on_disk, error;
	TEST_EXPECT(opennova::io::read_file_text(root + "/" + a, on_disk, error) && on_disk == "AAA\r\n");
	TEST_EXPECT(opennova::io::read_file_text(root + "/" + b, on_disk, error) && on_disk == "outside b, longer\r\n");
	const DocumentBase *b_document = session.document_base_for(b);
	TEST_EXPECT(b_document && b_document->unsaved());
	TEST_EXPECT(contains(v.activity.status, "1 could not be saved (b.wac)"));
	std::printf("save over: Keep my edits wrote its own file alone, the linked file's conflict refused and named\n");
	return 0;
}

// The stroke on the wire, its refusals while no planner is in, the brush and the pane.
int test_wire_and_workspace() {
	TerrainStrokeAsk ask;
	ask.plane = TerrainPlane::Surface;
	ask.brush.tool = TerrainTool::Surface;
	ask.brush.surface = 12;
	ask.brush.radius = 4.0f;
	ask.brush_named = kBrushTool | kBrushSurface | kBrushRadius;
	ask.points = {{10.0, -20.5}, {11.0, -21.0}};
	ask.gesture = 9;
	ask.end = false;
	const EditorRequest stroke = request::terrain_stroke("maps/00TRa.bms", ask);
	EditorRequest back;
	std::string error;
	TEST_EXPECT(editor_request_from_json(editor_request_to_json(stroke), back, error) && back == stroke);
	const EditorRequest end = request::end_edit("maps/00TRa.bms", 9, true);
	TEST_EXPECT(editor_request_from_json(editor_request_to_json(end), back, error) && back == end);
	// A stroke with no point, or past the brush's bounds, never reads.
	JsonValue bad;
	TEST_EXPECT(opennova::io::json_parse(R"({"kind":"terrain_stroke","stroke":{"plane":"heights","points":[]}})", bad, error));
	TEST_EXPECT(!editor_request_from_json(bad, back, error) && contains(error, "points"));
	TEST_EXPECT(opennova::io::json_parse(
			R"({"kind":"terrain_stroke","stroke":{"plane":"heights","points":[[0,0]],"brush":{"radius":9999}}})", bad, error));
	TEST_EXPECT(!editor_request_from_json(bad, back, error) && contains(error, "radius"));

	test_temp::TempDir dir("opennova_editor_terrain_brush_workspace");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Brush"));
	const SessionView &v = session.view();
	TEST_EXPECT(v.workspace.terrain_brush.tool == TerrainTool::Raise);
	session.handle(request::set_workspace(R"({"terrain_brush":{"tool":"lower","radius":8,"colour":"#80FF00","cursor":[1,2]}})"));
	TEST_EXPECT(session.outcome().done() && v.workspace.terrain_brush.tool == TerrainTool::Lower &&
	            v.workspace.terrain_brush.radius == 8.0f && v.workspace.terrain_brush.colour == 0x80FF00 &&
	            v.workspace.terrain_brush.has_cursor);
	session.handle(request::set_workspace(R"({"terrain_brush":{"radius":1000}})"));
	TEST_EXPECT(!session.outcome().done() && v.workspace.terrain_brush.radius == 8.0f);
	session.handle(request::set_workspace(R"({"terrain_brush":{"cursor":null}})"));
	TEST_EXPECT(session.outcome().done() && !v.workspace.terrain_brush.has_cursor);
	// A document that is neither a mission nor a terrain has no Terrain pane; a stroke in it is refused.
	const std::string root = v.project.root;
	TEST_EXPECT(test_io::write_text(root + "/scripts/a.wac", "aaa\r\n"));
	editor_test::handle_to_end(session, request::rescan());
	const std::string a = v.project.scan->find("a.wac")->relative_path;
	session.handle(request::open_document(a));
	session.handle(request::set_workspace(R"({"document":{"terrain":true}})"));
	TEST_EXPECT(!session.outcome().done() && !v.workspace.document(a).terrain);
	session.handle(request::terrain_stroke(a, ask));
	TEST_EXPECT(!session.outcome().done() && session.outcome().findings.size() == 1 &&
	            session.outcome().findings[0].code() == "terrain.stroke");
	// The terrain query answers the brush.
	JsonValue args = JsonValue::make_object();
	args.set("op", opennova::io::json_string("brush"));
	error.clear();
	const JsonValue refused = session.query("terrain", args, error);
	TEST_EXPECT(refused.is_null() && contains(error, "neither a mission nor a terrain"));
	std::printf("wire and workspace: the stroke read back, its refusals, the brush and the pane\n");
	return 0;
}

} // namespace

int main() {
	if (test_marks() != 0) return 1;
	if (test_linked_undo() != 0) return 1;
	if (test_scope_save_over() != 0) return 1;
	return test_wire_and_workspace();
}
