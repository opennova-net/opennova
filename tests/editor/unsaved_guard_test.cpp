// Pins the unsaved-changes prompt as the request table drives it (UnsavedGuard, ADR 0046 S13 A2):
// for every request kind, over a project whose extra.mnu has unsaved edits, a request of the kind
// that would lose, pack or write over them waits on the prompt exactly when its row's guard column
// says, the prompt naming what waits (a Close's or a Reload's document, another request's path),
// listing that file and offering Discard as the row's can_discard says; a guarded request that
// touches no unsaved file goes ahead; and no other kind ever opens the prompt.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <editor/assets/asset_import.h>
#include <editor/assets/asset_type_registry.h>
#include <editor/graph/asset_graph.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_kinds.h>
#include <editor/session/session_json.h>
#include <editor/session/session_view.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
namespace fs = std::filesystem;

namespace {

// A new project with its required files and a menu of its own, extra.mnu, open with an unsaved
// edit (its MAIN window's left edge moved).
struct Dirty {
	editor_test::TempProjectDir dir;
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session;
	std::string extra;
	explicit Dirty(const char *name) : dir(name), session(platform, preferences) {
		session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Guard"));
		editor_test::create_missing_files(session);
		session.handle(make_request(EditorRequestKind::CreateFile, "extra.mnu", asset_kind_token(AssetKind::Menu)));
		Document *document = session.document_for("extra.mnu");
		if (!document) return;
		extra = document->path();
		EditorRequest set = make_request(EditorRequestKind::EditRecord, extra);
		set.edit.address = document->address_at("0/window:0");
		set.edit.field = "position.left";
		set.edit.value = int64_t(8);
		session.handle(set);
	}
	const SessionView &view() const { return session.view(); }
	bool ready() { return !extra.empty() && session.documents_dirty() && session.document_for(extra)->dirty(); }
};

// The requirement row whose file is `name`, or null.
const RequirementRow *requirement_of(const SessionView &view, const std::string &name) {
	for (const RequirementRow &row : view.requirements.rows)
		if (normalized_logical_name(row.name) == normalized_logical_name(name)) return &row;
	return nullptr;
}

// A request of `kind` that would touch the unsaved extra.mnu where a request of the kind can:
// a project switch, Quit, Build and Play do by what they are; a Close or a Reload names it; an
// import brings a file of its name, replacing it; a rename renames it, an assignment renames it to
// a required file the project lacks, a rename everywhere renames the screen it defines. Any other
// kind names it where it names a document.
EditorRequest touching(EditorRequestKind kind, Dirty &dirty) {
	EditorRequest request = make_request(kind, dirty.extra);
	switch (kind) {
	case EditorRequestKind::NewProject:
		request.path = dirty.dir.file("elsewhere");
		request.text = "Elsewhere";
		break;
	case EditorRequestKind::OpenProject: request.path = dirty.view().project_root; break;
	case EditorRequestKind::ImportFiles: {
		const std::string loose = dirty.dir.file("loose/extra.mnu");
		const std::string saved = dirty.view().project_root + "/" + dirty.extra;
		std::error_code ec;
		fs::create_directories(fs::path(loose).parent_path(), ec);
		fs::copy_file(saved, loose, fs::copy_options::overwrite_existing, ec);
		std::vector<Diagnostic> diagnostics;
		request.path.clear();
		request.imports = list_import_sources({loose}, diagnostics);
		request.flag = true; // replace the project's file of the name
		break;
	}
	case EditorRequestKind::RenameAsset: request.text = "renamed.mnu"; break;
	case EditorRequestKind::AssignRequirement: {
		// main.mnu gone from the project: extra.mnu is a menu that can be it.
		const RequirementRow *row = requirement_of(dirty.view(), "main.mnu");
		if (!row) break;
		const std::string role = row->role;
		std::error_code ec;
		fs::remove(dirty.view().project_root + "/" + row->asset_path, ec);
		dirty.session.handle(make_request(EditorRequestKind::Rescan));
		request.text = role;
		break;
	}
	case EditorRequestKind::RenameSymbol: {
		for (const GraphSymbol *symbol : dirty.view().graph->symbols_of_kind(ReferenceKind::MenuScreen)) {
			if (symbol->file != dirty.extra) continue;
			request.text = symbol->locator;
			request.edit.field = symbol->field;
			request.edit.value = std::string("RENAMED");
		}
		break;
	}
	default: break;
	}
	return request;
}

} // namespace

// Every kind's guard column is what the prompt does.
static int test_guard_column_is_the_prompt() {
	size_t prompted = 0, went_ahead = 0;
	for (size_t i = 0; i < kEditorRequestKindCount; ++i) {
		const EditorRequestKind kind = static_cast<EditorRequestKind>(i);
		const RequestKindRow &row = request_kind_row(kind);
		Dirty dirty("opennova_editor_unsaved_guard");
		TEST_EXPECT(dirty.ready());
		const EditorRequest request = touching(kind, dirty);
		TEST_EXPECT(dirty.session.document_for(dirty.extra)->dirty());
		dirty.session.handle(request);
		const SessionView::UnsavedPrompt &prompt = dirty.view().unsaved_prompt;
		const bool guarded = row.guard != GuardScope::None;
		if (prompt.open != guarded)
			std::fprintf(stderr, "%s: the prompt %s, its guard column says %s\n", editor_request_kind_token(kind),
			             prompt.open ? "opened" : "did not open", guarded ? "it guards" : "none");
		TEST_EXPECT(prompt.open == guarded);
		if (!guarded) {
			++went_ahead;
			continue;
		}
		++prompted;
		TEST_EXPECT(dirty.session.outcome().unsaved_prompt && !dirty.session.outcome().done());
		TEST_EXPECT(prompt.action == kind);
		TEST_EXPECT(prompt.files == std::vector<std::string>({dirty.extra}));
		TEST_EXPECT(prompt.can_discard == row.can_discard);
		TEST_EXPECT(prompt.target == (row.guard == GuardScope::Document ? dirty.extra : request.path));
	}
	TEST_EXPECT(prompted == 12 && went_ahead == kEditorRequestKindCount - 12);
	return 0;
}

// A guarded request that touches no unsaved file goes ahead: a Close or a Reload of a clean
// document, a rename of a clean file that names none of the unsaved one's references.
static int test_untouched_goes_ahead() {
	Dirty dirty("opennova_editor_unsaved_guard_clean");
	TEST_EXPECT(dirty.ready());
	dirty.session.handle(make_request(EditorRequestKind::OpenDocument, "main.mnu"));
	const std::string main = dirty.session.document_for("main.mnu")->path();
	dirty.session.handle(make_request(EditorRequestKind::ReloadDocument, main));
	TEST_EXPECT(!dirty.view().unsaved_prompt.open && dirty.session.outcome().done());
	dirty.session.handle(make_request(EditorRequestKind::CloseDocument, main));
	TEST_EXPECT(!dirty.view().unsaved_prompt.open && dirty.session.outcome().done() && !dirty.session.document_for(main));
	dirty.session.handle(make_request(EditorRequestKind::RenameAsset, "menu_style.mns", "renamed.mns"));
	TEST_EXPECT(!dirty.view().unsaved_prompt.open);
	TEST_EXPECT(dirty.session.document_for(dirty.extra)->dirty());
	return 0;
}

int main() {
	int failures = 0;
	failures += test_guard_column_is_the_prompt();
	failures += test_untouched_goes_ahead();
	if (failures == 0) std::printf("editor_unsaved_guard: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
