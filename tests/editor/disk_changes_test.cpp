// ADR 0046 DI-01, outside saves come back: what another program changes of a project's files found
// without a Rescan (editor/assets/disk_changes.h, editor/session/disk_watch.h). The looks: a file that
// moved waits until a later look finds it holding still (250 ms on the session's clock), one whose last
// write already settled is read at the first look, one gone too, one that moved back forgotten; the
// folders the walk went into, each with its stamp, listed again only when it moved (a new file, a gone
// one, a new folder, a gone folder; a dot-folder and the export folder never); the sweep over every file,
// a step at a time. The session: an open catalog written from outside read again within the hold, its
// selection kept by its place, with no Rescan; a file not open found by the focus-in sweep; a new file
// and a gone one found by the folders, Output naming each; an open document with unsaved edits raising
// its conflict at once with its two fixes, Keep my edits behind a confirmation, Save refused until then;
// a PNG the import round trip watches keeping S18's two-second rule.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <base/io/file_time.h>
#include <editor/assets/disk_changes.h>
#include <editor/assets/project_scan.h>
#include <editor/model/document.h>
#include <editor/project/project_files.h>
#include <editor/session/preferences_store.h>
#include <editor/session/problem_confirmation.h>
#include <editor/session/problem_fixes.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/png/png_encode.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova;
using namespace opennova::editor;
namespace fs = std::filesystem;
using opennova::png::encode_png_rgba;

namespace {

std::string items_text(int count) {
	std::string text;
	for (int i = 0; i < count; ++i)
		text += "begin \"Crate " + std::to_string(i) + "\"\nid " + std::to_string(100100 + i) + "\ntype building\nhp " +
		        std::to_string(10 + i) + "\nend\n";
	return text;
}

bool contains(const std::vector<std::string> &list, const std::string &item) {
	for (const std::string &each : list)
		if (each == item) return true;
	return false;
}

// Whether Output holds a line (or a line under one) holding `text`.
bool output_says(const SessionView &view, const std::string &text) {
	const OutputLog &output = view.activity.output;
	for (size_t i = 0; i < output.size(); ++i) {
		if (output[i].find(text) != std::string::npos) return true;
		for (const std::string &folded : output.folded(i))
			if (folded.find(text) != std::string::npos) return true;
	}
	return false;
}

// The finding of `code` on `path` among the Problems rows (SIZE_MAX: none).
size_t finding_at(const SessionView &view, const char *code, const std::string &path) {
	for (size_t i = 0; i < view.findings.diagnostics.size(); ++i)
		if (view.findings.diagnostics[i].code() == code && view.findings.diagnostics[i].asset == path) return i;
	return SIZE_MAX;
}

int test_looks() {
	editor_test::TempProjectDir dir{"opennova_editor_disk_looks"};
	const std::string root = dir.root();
	ProjectDocument doc;
	doc.title = "Looks";
	const ProjectPaths paths = ProjectPaths::for_root(root);
	TEST_EXPECT(editor_test::write_text(root + "/a.def", items_text(1)) &&
	            editor_test::write_text(root + "/sub/b.def", items_text(2)) &&
	            editor_test::write_text(root + "/sub/deep/c.def", items_text(1)) &&
	            editor_test::write_text(root + "/.opennova/cache.def", items_text(1)));
	TEST_EXPECT(editor_test::backdate_tree(root, std::chrono::seconds(60)));
	const AssetScan scan = scan_project_assets(paths, doc);
	// The walk keeps each folder it went into with its stamp, and each file's own stamp.
	TEST_EXPECT(scan.folders().count("") && scan.folders().count("sub") && scan.folders().count("sub/deep") &&
	            !scan.folders().count(".opennova"));
	TEST_EXPECT(scanned_stamp(scan, "sub/b.def") == disk_stamp(paths, "sub/b.def") &&
	            scanned_stamp(scan, "sub/b.def").present && !scanned_stamp(scan, "nothing.def").present);
	DiskChanges changes;
	const int64_t now = io::file_clock_now_ticks();
	size_t listed = 0;
	// Nothing changed: the folders' stamps settled long ago, none listed.
	TEST_EXPECT(editor_test::backdate(root, std::chrono::seconds(60)));
	{
		std::error_code ec;
		fs::last_write_time(system_path(root + "/sub"), fs::file_time_type::clock::now() - std::chrono::seconds(60), ec);
		fs::last_write_time(system_path(root + "/sub/deep"), fs::file_time_type::clock::now() - std::chrono::seconds(60), ec);
	}
	const AssetScan settled = scan_project_assets(paths, doc);
	TEST_EXPECT(changes.folder_changes(paths, doc, settled, now, &listed).empty() && listed == 0);

	// A file written just now waits until a look 250 ms later finds it as it was.
	TEST_EXPECT(editor_test::write_text(root + "/a.def", items_text(3)));
	changes.look_at(paths, settled, {"a.def"}, 1000, io::file_clock_now_ticks());
	TEST_EXPECT(changes.waiting() == 1 && changes.ready() == 0);
	changes.look_at(paths, settled, {"a.def"}, 1100, io::file_clock_now_ticks());
	TEST_EXPECT(changes.waiting() == 1 && changes.ready() == 0);
	changes.look_at(paths, settled, {"a.def"}, 1000 + kDiskHoldStillMs, io::file_clock_now_ticks());
	TEST_EXPECT(changes.waiting() == 0 && changes.take_ready() == std::vector<std::string>({"a.def"}));
	// Moved again between two looks: it waits from the second.
	TEST_EXPECT(editor_test::write_text(root + "/a.def", items_text(4)));
	changes.look_at(paths, settled, {"a.def"}, 2000, io::file_clock_now_ticks());
	TEST_EXPECT(editor_test::write_text(root + "/a.def", items_text(5)));
	changes.look_at(paths, settled, {"a.def"}, 2300, io::file_clock_now_ticks());
	TEST_EXPECT(changes.waiting() == 1 && changes.ready() == 0);
	// Its last write settled (two seconds back): ready at the first look.
	TEST_EXPECT(editor_test::backdate(root + "/a.def", std::chrono::seconds(60)));
	changes.look_at(paths, settled, {"a.def"}, 2400, io::file_clock_now_ticks());
	TEST_EXPECT(changes.waiting() == 0 && changes.take_ready() == std::vector<std::string>({"a.def"}));
	// Gone: it waits as any change does, then is ready.
	{
		std::error_code ec;
		fs::remove(system_path(root + "/sub/deep/c.def"), ec);
	}
	changes.look_at(paths, settled, {"sub/deep/c.def"}, 3000, io::file_clock_now_ticks());
	changes.look_at(paths, settled, {"sub/deep/c.def"}, 3000 + kDiskHoldStillMs, io::file_clock_now_ticks());
	TEST_EXPECT(changes.take_ready() == std::vector<std::string>({"sub/deep/c.def"}));
	// Moved back to what the scan holds: forgotten.
	TEST_EXPECT(editor_test::write_text(root + "/sub/b.def", items_text(9)));
	changes.look_at(paths, settled, {"sub/b.def"}, 4000, io::file_clock_now_ticks());
	TEST_EXPECT(changes.waiting() == 1);
	TEST_EXPECT(editor_test::write_text(root + "/sub/b.def", items_text(2)));
	{
		std::error_code ec;
		fs::last_write_time(system_path(root + "/sub/b.def"),
		                    fs::file_time_type(fs::file_time_type::duration(scanned_stamp(settled, "sub/b.def").modified_ticks)), ec);
	}
	changes.look_at(paths, settled, {"sub/b.def"}, 4100, io::file_clock_now_ticks());
	TEST_EXPECT(changes.waiting() == 0 && changes.ready() == 0);

	// The folders: a file made in one, one gone from another, a folder made with a file, a folder gone with
	// what it held; a dot-folder's file and the export folder's never.
	TEST_EXPECT(editor_test::write_text(root + "/new.def", items_text(1)) &&
	            editor_test::write_text(root + "/made/d.def", items_text(1)) &&
	            editor_test::write_text(root + "/.opennova/other.def", items_text(1)) &&
	            editor_test::write_text(paths.export_dir(doc) + "/shipped.def", items_text(1)));
	{
		std::error_code ec;
		fs::remove(system_path(root + "/sub/b.def"), ec);
		fs::remove_all(system_path(root + "/sub/deep"), ec);
	}
	const std::vector<std::string> found = changes.folder_changes(paths, doc, settled, io::file_clock_now_ticks(), &listed);
	TEST_EXPECT(found == std::vector<std::string>({"made/d.def", "new.def", "sub/b.def", "sub/deep/c.def"}));
	TEST_EXPECT(listed == 4); // the project's, build/ and made/ (new: build/ holds the export folder) and sub/
	// Listed again only while a stamp may still move in its tick: settled, none.
	TEST_EXPECT(editor_test::backdate(root, std::chrono::seconds(60)));
	{
		std::error_code ec;
		fs::last_write_time(system_path(root + "/sub"), fs::file_time_type::clock::now() - std::chrono::seconds(60), ec);
		fs::last_write_time(system_path(root + "/made"), fs::file_time_type::clock::now() - std::chrono::seconds(60), ec);
		fs::last_write_time(system_path(root + "/build"), fs::file_time_type::clock::now() - std::chrono::seconds(60), ec);
	}
	changes.folder_changes(paths, doc, settled, io::file_clock_now_ticks(), &listed); // the stamps taken anew
	TEST_EXPECT(changes.folder_changes(paths, doc, settled, io::file_clock_now_ticks(), &listed).empty() && listed == 0);

	// The sweep: every file of a scan looked at, a step at a time.
	const AssetScan now_scan = scan_project_assets(paths, doc);
	TEST_EXPECT(editor_test::write_text(root + "/made/d.def", items_text(7)) &&
	            editor_test::backdate(root + "/made/d.def", std::chrono::seconds(60)));
	std::vector<std::string> every;
	for (const auto &[path, visit] : now_scan.visits()) every.push_back(path);
	DiskChanges sweep;
	sweep.begin_sweep(every);
	size_t steps = 0;
	while (sweep.sweeping()) {
		sweep.step_sweep(paths, now_scan, kWalkEntryCost, 5000, io::file_clock_now_ticks());
		++steps;
	}
	TEST_EXPECT(steps == every.size() && every.size() == 3);
	TEST_EXPECT(sweep.take_ready() == std::vector<std::string>({"made/d.def"}));
	std::printf("looks: a file read once it holds still or settled, gone, moved back; the folders' made and gone "
	            "files, a dot-folder and the export folder passed over; the sweep a step at a time\n");
	return 0;
}

int test_session() {
	editor_test::TempProjectDir dir{"opennova_editor_disk_watch"};
	editor_test::FakePlatform platform;
	platform.clock = 10000;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Outside"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	const std::string root = view.project.root;
	TEST_EXPECT(editor_test::write_text(root + "/defs/items.def", items_text(2)) &&
	            editor_test::write_text(root + "/defs/other.def", items_text(1)) &&
	            editor_test::backdate_tree(root, std::chrono::seconds(60)));
	editor_test::handle_to_end(session, request::rescan());
	editor_test::handle_to_end(session, request::open_document("defs/items.def"));
	const Document *items = session.document_for("defs/items.def");
	TEST_EXPECT(items && items->rows().size() == 2);
	if (!items || items->rows().size() != 2) return 1;
	const NodeAddress second{items->rows()[1]->id, items->rows()[1]->kind, 0};
	session.handle(request::select_record("defs/items.def", second));
	TEST_EXPECT(view.documents.selection.primary == second);
	// Nothing changed: nothing done, and the view's clock stands (a client polling with since sees nothing).
	session.handle(request::refresh_changed_sources());
	const uint64_t clock = view.revisions.any();
	session.handle(request::refresh_changed_sources());
	TEST_EXPECT(session.outcome().done() && session.outcome().operation == 0 && view.project.outside_waiting == 0 &&
	            view.revisions.any() == clock);

	// Another program saves the open catalog: written just now, it waits for a look that finds it holding
	// still, then comes back with no Rescan, the record selected in it selected again by its place.
	TEST_EXPECT(editor_test::write_text(root + "/defs/items.def", items_text(3)));
	session.handle(request::refresh_changed_sources());
	TEST_EXPECT(session.outcome().operation == 0 && view.project.outside_waiting == 1);
	platform.clock += 100;
	session.handle(request::refresh_changed_sources());
	TEST_EXPECT(session.outcome().operation == 0 && view.project.outside_waiting == 1);
	platform.clock += kDiskHoldStillMs;
	editor_test::handle_to_end(session, request::refresh_changed_sources());
	TEST_EXPECT(session.outcome().operation != 0 && view.project.outside_waiting == 0);
	items = session.document_for("defs/items.def");
	TEST_EXPECT(items && items->rows().size() == 3 && !items->dirty());
	if (!items || items->rows().size() != 3) return 1;
	TEST_EXPECT(view.documents.selection.primary.row == items->rows()[1]->id &&
	            items->locator(view.documents.selection.primary) == "1");
	TEST_EXPECT(output_says(view, "Reloaded defs/items.def: it changed outside the editor."));
	// A Reload keeps the selection the same way.
	session.handle(request::select_record("defs/items.def", {items->rows()[2]->id, items->rows()[2]->kind, 0}));
	editor_test::handle_to_end(session, request::reload_document("defs/items.def"));
	items = session.document_for("defs/items.def");
	TEST_EXPECT(items && items->rows().size() == 3 && items->locator(view.documents.selection.primary) == "2");
	if (!items || items->rows().size() != 3) return 1;
	// The editor's own Save is no outside change: nothing comes back.
	{
		EditorRequest own = request::edit_record("defs/items.def", Edit());
		own.edits[0].address = {items->rows()[0]->id, items->rows()[0]->kind, 0};
		own.edits[0].field = "hp";
		own.edits[0].value = int64_t(55);
		session.handle(own);
		editor_test::handle_to_end(session, request::save("defs/items.def"));
		TEST_EXPECT(!items->dirty());
		session.handle(request::refresh_changed_sources());
		platform.clock += kDiskHoldStillMs;
		session.handle(request::refresh_changed_sources());
		TEST_EXPECT(session.outcome().operation == 0 && view.project.outside_waiting == 0);
	}

	// A file no document shows changes: the checks pass it by; the sweep a focus-in begins finds it (its last
	// write settled: ready at once), and the next check reads it again.
	TEST_EXPECT(editor_test::write_text(root + "/defs/other.def", items_text(4)) &&
	            editor_test::backdate(root + "/defs/other.def", std::chrono::seconds(30)));
	session.handle(request::refresh_changed_sources());
	TEST_EXPECT(session.outcome().operation == 0 && view.project.outside_waiting == 0);
	session.handle(request::refresh_changed_sources(true));
	TEST_EXPECT(view.project.outside_sweeping);
	session.set_poll_budget(PollBudget{0, kWalkEntryCost});
	size_t polls = 0;
	while (view.project.outside_sweeping && polls < 10000) {
		session.poll();
		++polls;
	}
	TEST_EXPECT(!view.project.outside_sweeping && polls > 1 && view.project.outside_waiting == 1);
	editor_test::handle_to_end(session, request::refresh_changed_sources());
	TEST_EXPECT(session.outcome().operation != 0 && view.project.outside_waiting == 0);
	TEST_EXPECT(output_says(view, "Read defs/other.def again: it changed outside the editor."));
	TEST_EXPECT(scanned_stamp(*view.project.scan, "defs/other.def") == disk_stamp(ProjectPaths::for_root(root), "defs/other.def"));

	// A file made and one deleted: the folder's last write moved, so the checks find both, each named.
	TEST_EXPECT(editor_test::write_text(root + "/defs/new.def", items_text(1)));
	{
		std::error_code ec;
		fs::remove(system_path(root + "/defs/other.def"), ec);
	}
	session.handle(request::refresh_changed_sources());
	TEST_EXPECT(session.outcome().operation == 0 && view.project.outside_waiting == 2);
	platform.clock += kDiskHoldStillMs;
	editor_test::handle_to_end(session, request::refresh_changed_sources());
	TEST_EXPECT(session.outcome().operation != 0 && view.project.outside_waiting == 0);
	TEST_EXPECT(view.project.scan->at_path("defs/new.def") && !view.project.scan->at_path("defs/other.def"));
	TEST_EXPECT(output_says(view, "2 files changed outside the editor came back.") &&
	            output_says(view, "Found defs/new.def: it was made outside the editor.") &&
	            output_says(view, "defs/other.def is gone: it was deleted outside the editor."));

	// Unsaved edits over a file another program saves: kept, the conflict raised at once with its two fixes.
	EditorRequest edit = request::edit_record("defs/items.def", Edit());
	edit.edits[0].address = {items->rows()[0]->id, items->rows()[0]->kind, 0};
	edit.edits[0].field = "hp";
	edit.edits[0].value = int64_t(77);
	session.handle(edit);
	TEST_EXPECT(items->dirty());
	TEST_EXPECT(editor_test::write_text(root + "/defs/items.def", items_text(5)) &&
	            editor_test::backdate(root + "/defs/items.def", std::chrono::seconds(30)));
	editor_test::handle_to_end(session, request::refresh_changed_sources());
	TEST_EXPECT(session.outcome().operation != 0);
	TEST_EXPECT(session.document_for("defs/items.def") == items && items->dirty() && items->rows().size() == 3);
	const size_t conflict = finding_at(view, "document.conflict", "defs/items.def");
	TEST_EXPECT(conflict != SIZE_MAX && output_says(view, "defs/items.def changed outside the editor while it has unsaved edits"));
	if (conflict == SIZE_MAX) return 1;
	const std::vector<ProblemFix> fixes = fixes_for(view.findings.diagnostics[conflict], view);
	TEST_EXPECT(fixes.size() == 2 && fixes[0].label == "Reload items.def" &&
	            fixes[1].label == "Keep my edits and save over it");
	TEST_EXPECT(fixes.size() == 2 && !fix_asks_first(fixes[0]) && fix_asks_first(fixes[1]) &&
	            fixes[1].request.kind == EditorRequestKind::Save && fixes[1].request.force);
	// A plain Save is refused while it stands.
	session.handle(request::save("defs/items.def"));
	TEST_EXPECT(items->dirty() && session.outcome().findings.size() == 1 &&
	            session.outcome().findings[0].code() == "document.conflict");
	// Keep my edits, confirmed (the workspace's confirmation, as Problems asks it), writes over the file.
	const size_t still = finding_at(view, "document.conflict", "defs/items.def");
	TEST_EXPECT(still != SIZE_MAX);
	TEST_EXPECT(session.handle(request::set_workspace(
	        "{\"problems\": {\"confirm\": {\"finding\": " + std::to_string(still) +
	        ", \"label\": \"Keep my edits and save over it\"}}}")));
	TEST_EXPECT(view.workspace.problems.confirm.open());
	editor_test::handle_to_end(session, request::apply_confirmation());
	TEST_EXPECT(!items->dirty() && finding_at(view, "document.conflict", "defs/items.def") == SIZE_MAX);
	std::string text, error;
	TEST_EXPECT(io::read_file_text(root + "/defs/items.def", text, error) && text.find("hp 77") != std::string::npos &&
	            text.find("Crate 4") == std::string::npos);
	TEST_EXPECT(output_says(view, "Saved defs/items.def over what changed outside the editor"));

	// A PNG the game reads as it is: the import round trip's own, two seconds after its last write (S18).
	{
		std::vector<uint8_t> rgba(64, 120);
		TEST_EXPECT(editor_test::write_bytes(root + "/textures/pic.png", encode_png_rgba(rgba.data(), 4, 4)) &&
		            editor_test::backdate(root + "/textures/pic.png", std::chrono::seconds(60)));
		editor_test::handle_to_end(session, request::rescan());
		editor_test::handle_to_end(session, request::open_document("textures/pic.png"));
		std::vector<uint8_t> red;
		for (int i = 0; i < 16; ++i) red.insert(red.end(), {220, 10, 10, 255});
		TEST_EXPECT(editor_test::write_bytes(root + "/textures/pic.png", encode_png_rgba(red.data(), 4, 4)));
		session.handle(request::refresh_changed_sources());
		platform.clock += kDiskHoldStillMs;
		session.handle(request::refresh_changed_sources());
		TEST_EXPECT(session.outcome().operation == 0 && view.project.outside_waiting == 1);
		TEST_EXPECT(editor_test::backdate(root + "/textures/pic.png", std::chrono::seconds(60)));
		editor_test::handle_to_end(session, request::refresh_changed_sources());
		TEST_EXPECT(session.outcome().operation != 0 && view.project.outside_waiting == 0);
	}
	std::printf("session: an open catalog's outside save read again with its selection kept, the sweep's file, "
	            "a made and a gone file named, a conflict raised at once with Reload and Keep my edits, a PNG "
	            "keeping S18's rule\n");
	return 0;
}

} // namespace

int main() {
	int failures = test_looks();
	failures += test_session();
	if (failures == 0) std::printf("editor_disk_changes: all passed\n");
	return failures == 0 ? 0 : 1;
}
