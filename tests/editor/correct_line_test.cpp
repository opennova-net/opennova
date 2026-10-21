// A blocked file is never a dead end (session/source_line.h; session/problem_fixes.h, Correct the line...): a line of
// a file's source the model cannot carry is a source line (Diagnostic::source_line), whose Problems row offers
// Correct the line..., which opens the workspace's correct_line on it, its text as the file holds it; correct_line
// writes the line in the file on disk, its line end kept and every other byte as it was (an empty text taking the
// line out), and the document is read again, unblocked once no blocking line stands. Lines as the finding counts
// them: CR LF pairs for a kind whose game reader ends a line there alone (an LF a byte of its line), LF for any
// other. Refused, nothing written: a line the file has not, one changed since, a text holding a line end.
#include <cstdio>
#include <string>
#include <vector>

#include <base/io/file_io.h>
#include <editor/model/document.h>
#include <editor/session/preferences_store.h>
#include <editor/session/problem_fixes.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/source_line.h>
#include <editor/session/view/session_view.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/temp_dir.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova;
using namespace opennova::editor;

namespace {

std::string line_of(const std::string &bytes, size_t line, bool crlf_only) {
	SourceLine at;
	size_t lines = 0;
	return find_source_line(bytes, line, crlf_only, at, lines) ? bytes.substr(at.begin, at.end - at.begin) : "<none>";
}

std::string corrected(const std::string &bytes, size_t line, bool crlf_only, const std::string &text) {
	SourceLine at;
	size_t lines = 0;
	return find_source_line(bytes, line, crlf_only, at, lines) ? with_source_line(bytes, at, text) : "<none>";
}

// The line walk and the rewrite alone.
int test_lines() {
	const std::string crlf = "a\r\nb\nc\r\nlast";
	// CR LF alone: an LF is a byte of its line, a last line with no pair a line.
	TEST_EXPECT(line_of(crlf, 1, true) == "a" && line_of(crlf, 2, true) == "b\nc" && line_of(crlf, 3, true) == "last" &&
	            line_of(crlf, 4, true) == "<none>");
	// LF: a CR before it part of the ending.
	TEST_EXPECT(line_of(crlf, 2, false) == "b" && line_of(crlf, 3, false) == "c" && line_of(crlf, 4, false) == "last");
	TEST_EXPECT(corrected(crlf, 1, true, "x y") == "x y\r\nb\nc\r\nlast");
	TEST_EXPECT(corrected(crlf, 3, true, "end") == "a\r\nb\nc\r\nend");
	TEST_EXPECT(corrected(crlf, 1, true, "") == "b\nc\r\nlast");
	TEST_EXPECT(corrected(crlf, 3, true, "") == "a\r\nb\nc\r\n");
	TEST_EXPECT(corrected("one\ntwo\n", 2, false, "2") == "one\n2\n");
	SourceLine at;
	size_t lines = 0;
	TEST_EXPECT(!find_source_line("a\r\nb\r\n", 3, true, at, lines) && lines == 2);
	TEST_EXPECT(source_line_crlf_only(AssetKind::ItemDefs));
	return 0;
}

// A minted blocked items.def through the session: its row's fix, the dialog it opens, the line corrected on disk,
// the document read again unblocked.
int test_session() {
	test_temp::TempDir dir("opennova_editor_correct_line");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Lines"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	const AssetEntry *items = view.project.scan->find("items.def");
	TEST_EXPECT(items != nullptr);
	if (!items) return 1;
	const std::string path = items->relative_path;
	const std::string file = view.project.root + "/" + path;
	// Two attachment angles: the other two read slots an earlier line left, which no save can write.
	const std::string blocked = "begin \"Gun\"\r\nid 100001\r\naddeweap ewep01 100201 15 37\r\nend\r\n";
	TEST_EXPECT(test_io::write_file(file, std::vector<uint8_t>(blocked.begin(), blocked.end())));
	editor_test::handle_to_end(session, request::rescan());
	session.handle(request::open_document(path));
	session.run_operations();
	const DocumentBase *open = session.document_base_for(path);
	TEST_EXPECT(open && open->blocked());
	const Diagnostic *row = nullptr;
	for (const Diagnostic &d : view.findings.diagnostics)
		if (d.asset == path && d.source_line && d.line == 3) row = &d;
	TEST_EXPECT(row != nullptr);
	if (!row) return 1;
	const std::vector<ProblemFix> fixes = fixes_for(*row, view);
	const ProblemFix *fix = nullptr;
	for (const ProblemFix &each : fixes)
		if (each.label == "Correct the line...") fix = &each;
	TEST_EXPECT(fix && fix->request.kind == EditorRequestKind::CorrectLine && fix->request.ask_name && !fix->bulk &&
	            fix->detail.find("cannot be undone") != std::string::npos);
	if (!fix) return 1;
	// The fix opens the dialog on the line, its text as the file holds it; nothing is written.
	session.handle(fix->request);
	const WorkspaceView::CorrectLine &dialog = view.workspace.correct_line;
	TEST_EXPECT(dialog.path == path && dialog.line == 3 && dialog.was == "addeweap ewep01 100201 15 37" &&
	            dialog.text == dialog.was);
	const std::string was = dialog.was;
	std::string text, message;
	TEST_EXPECT(io::read_file_text(file, text, message) && text == blocked);
	// Refused, nothing written: a line changed since, a text holding a line end, a line the file has not.
	session.handle(request::correct_line(path, 3, "addeweap ewep01 0", "addeweap other"));
	TEST_EXPECT(session.outcome().refused && session.outcome().findings.back().code() == "document.source_line");
	session.handle(request::correct_line(path, 3, "a\r\nb", was));
	TEST_EXPECT(session.outcome().refused);
	session.handle(request::correct_line(path, 9, "x"));
	TEST_EXPECT(session.outcome().refused && session.outcome().findings.back().message.find("has 4 lines") != std::string::npos);
	TEST_EXPECT(io::read_file_text(file, text, message) && text == blocked);
	// Applied: the line written, its CR LF and every other byte kept; the dialog closed; the document read again,
	// unblocked.
	editor_test::handle_to_end(session, request::correct_line(path, 3, "addeweap ewep01 100201 15 37 0 0", was));
	TEST_EXPECT(session.outcome().done() && view.workspace.correct_line.path.empty());
	TEST_EXPECT(io::read_file_text(file, text, message) &&
	            text == "begin \"Gun\"\r\nid 100001\r\naddeweap ewep01 100201 15 37 0 0\r\nend\r\n");
	open = session.document_base_for(path);
	TEST_EXPECT(open && !open->blocked());
	for (const Diagnostic &d : view.findings.diagnostics) TEST_EXPECT(!(d.asset == path && d.source_line));
	// Taken out: an empty text removes the line and its line end.
	editor_test::handle_to_end(session, request::correct_line(path, 2, "", "id 100001"));
	TEST_EXPECT(io::read_file_text(file, text, message) &&
	            text == "begin \"Gun\"\r\naddeweap ewep01 100201 15 37 0 0\r\nend\r\n");
	// The game's text is the code page's bytes: a line holding 0xE9 reads as UTF-8 "é" in the dialog and over the
	// wire, a text typed with it is written as the one byte, and a character the page has none for is refused.
	const std::string accented = "begin \"Caf\xE9\"\r\nid 100001\r\naddeweap ewep01 100201 15 37\r\nend\r\n";
	TEST_EXPECT(test_io::write_file(file, std::vector<uint8_t>(accented.begin(), accented.end())));
	editor_test::handle_to_end(session, request::rescan());
	session.handle(request::correct_line(path, 1, {}, std::nullopt, true));
	TEST_EXPECT(view.workspace.correct_line.was == "begin \"Caf\xC3\xA9\"");
	session.handle(request::correct_line(path, 1, "begin \"Gun \xE2\x86\x92\"", std::string("begin \"Caf\xC3\xA9\"")));
	TEST_EXPECT(session.outcome().refused);
	editor_test::handle_to_end(session, request::correct_line(path, 1, "begin \"Th\xC3\xA9\"", std::string("begin \"Caf\xC3\xA9\"")));
	TEST_EXPECT(io::read_file_text(file, text, message) &&
	            text == "begin \"Th\xE9\"\r\nid 100001\r\naddeweap ewep01 100201 15 37\r\nend\r\n");
	std::printf("correct line: a blocked items.def's row offers it, the dialog opens on the line, the file is written "
	            "and read again unblocked; the refusals; the code page's bytes as UTF-8 and back\n");
	return 0;
}

} // namespace

int main() {
	int failures = test_lines();
	failures += test_session();
	if (failures == 0) std::printf("editor_correct_line: all passed\n");
	return failures == 0 ? 0 : 1;
}
