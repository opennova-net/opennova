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
#include <editor/graph/rename_transaction.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/request_kinds.h>
#include <editor/session/session_json.h>
#include <editor/session/view/session_view.h>

#include <formats/tga/tga.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/menu_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
namespace fs = std::filesystem;

namespace {

// A new project with its required files and a menu of its own, extra.mnu, open with an unsaved
// edit (its MAIN window's left edge moved). The project's save_before_play off (DI-26: on, Play saves first
// and asks nothing), so every row the prompt guards asks.
struct Dirty {
	editor_test::TempProjectDir dir;
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session;
	std::string extra;
	explicit Dirty(const char *name) : dir(name), session(platform, preferences) {
		session.handle(request::new_project(dir.file("project"), "Guard"));
		session.run_operations();
		editor_test::create_missing_files(session);
		ProjectSettingsChange asking;
		asking.save_before_play = false;
		editor_test::apply_settings(session, asking);
		session.handle(request::create_file("extra.mnu", asset_kind_token(AssetKind::Menu)));
		Document *document = session.document_for("extra.mnu");
		if (!document) return;
		extra = document->path();
		Edit set;
		set.address = document->address_at("0/window:0");
		set.field = "position.left";
		set.value = int64_t(8);
		session.handle(request::edit_record(extra, set));
	}
	const SessionView &view() const { return session.view(); }
	bool ready() { return !extra.empty() && session.documents_dirty() && session.document_for(extra)->dirty(); }
};

// The requirement row whose file is `name`, or null.
const RequirementRow *requirement_of(const SessionView &view, const std::string &name) {
	for (const RequirementRow &row : view.project.requirements->rows)
		if (normalized_logical_name(row.name) == normalized_logical_name(name)) return &row;
	return nullptr;
}

// A request of `kind` that would touch the unsaved extra.mnu where a request of the kind can:
// a project switch, Quit, Build and Play do by what they are; a Close or a Reload names it; an
// import brings a file of its name, replacing it; a rename renames it, an assignment renames it to
// a required file the project lacks, a rename everywhere renames the screen it defines. Any other
// kind that takes a path (its row's params) names it there.
EditorRequest touching(EditorRequestKind kind, Dirty &dirty) {
	EditorRequest request = request::of(kind);
	if (request_kind_row(kind).params.has(RequestFieldId::Path)) request.path = dirty.extra;
	switch (kind) {
	case EditorRequestKind::NewProject:
		request.dir = dirty.dir.file("elsewhere");
		request.title = "Elsewhere";
		break;
	case EditorRequestKind::OpenProject: request.dir = dirty.view().project.root; break;
	case EditorRequestKind::ImportFiles: {
		const std::string loose = dirty.dir.file("loose/extra.mnu");
		const std::string saved = dirty.view().project.root + "/" + dirty.extra;
		std::error_code ec;
		fs::create_directories(fs::path(loose).parent_path(), ec);
		fs::copy_file(saved, loose, fs::copy_options::overwrite_existing, ec);
		std::vector<Diagnostic> diagnostics;
		request.imports = list_import_choices({loose}, diagnostics);
		request.replace = true; // replace the project's file of the name
		break;
	}
	case EditorRequestKind::RenameAsset: request.new_name = "renamed.mnu"; break;
	case EditorRequestKind::MoveAsset: request.folder = "moved"; break; // DI-03: its document reopens there
	// DI-25: the folder holding extra.mnu renamed (its document reopens there); the file history's last step
	// (extra.mnu's New file) taken back takes it to the trash, and a delete of it taken back and done again too.
	case EditorRequestKind::RenameFolder:
		request.folder = folder_of_path(dirty.extra);
		request.new_name = "renamed_folder";
		break;
	case EditorRequestKind::RedoFile: {
		dirty.session.handle(request::save(dirty.extra));
		editor_test::handle_to_end(dirty.session, request::delete_asset(dirty.extra, true));
		editor_test::handle_to_end(dirty.session, request::undo_file());
		dirty.session.handle(request::open_document(dirty.extra));
		if (Document *document = dirty.session.document_for(dirty.extra)) {
			Edit set;
			set.address = document->address_at("0/window:0");
			set.field = "position.left";
			set.value = int64_t(9);
			dirty.session.handle(request::edit_record(dirty.extra, set));
		}
		break;
	}
	case EditorRequestKind::SplitTexture: {
		// A texture extra.mnu's window shows (that edit unsaved too, validated into the graph): a split of it for
		// extra.mnu rewrites extra.mnu.
		const std::vector<uint8_t> rgba(16, 200);
		std::vector<uint8_t> tga;
		std::string why;
		opennova::tga::tga_write_rgba32(rgba.data(), 2, 2, tga, why);
		editor_test::write_bytes(dirty.view().project.root + "/textures/tex.tga", tga);
		dirty.session.handle(request::rescan());
		dirty.session.run_operations();
		Document *document = dirty.session.document_for(dirty.extra);
		if (!document) break;
		editor_test::handle_to_end(dirty.session, request::edit_record(dirty.extra, menu_test::image_edits(
				*document, document->address_at("0/window:0"), "tex.tga")));
		dirty.session.run_operations();
		request.path = "textures/tex.tga";
		request.new_name = "tex_2.tga";
		request.paths = {dirty.extra};
		break;
	}
	case EditorRequestKind::AssignRequirement: {
		// main.mnu gone from the project: extra.mnu is a menu that can be it.
		const RequirementRow *row = requirement_of(dirty.view(), "main.mnu");
		if (!row) break;
		const std::string role = row->role;
		std::error_code ec;
		fs::remove(dirty.view().project.root + "/" + row->asset_path, ec);
		dirty.session.handle(request::rescan());
		dirty.session.run_operations();
		request.role = role;
		break;
	}
	case EditorRequestKind::RenameSymbol: {
		for (const GraphSymbol *symbol : dirty.view().findings.graph->symbols_of_kind(ReferenceKind::MenuScreen)) {
			if (symbol->file != dirty.extra) continue;
			request.locator = symbol->locator;
			request.field = symbol->field;
			request.new_name = "RENAMED";
		}
		break;
	}
	case EditorRequestKind::RenameBack: {
		// The way back of a rename of the screen extra.mnu defines: its edit saved, the screen renamed, then
		// an edit made again (the way back rewrites extra.mnu).
		dirty.session.handle(request::save(dirty.extra));
		dirty.session.run_operations(); // validated: the graph holds the screen
		std::string locator, field;
		for (const GraphSymbol *symbol : dirty.view().findings.graph->symbols_of_kind(ReferenceKind::MenuScreen))
			if (symbol->file == dirty.extra) {
				locator = symbol->locator;
				field = symbol->field;
			}
		editor_test::handle_to_end(dirty.session, request::rename_symbol(dirty.extra, locator, field, "RENAMED"));
		if (Document *document = dirty.session.document_for(dirty.extra)) {
			Edit set;
			set.address = document->address_at("0/window:0");
			set.field = "position.left";
			set.value = int64_t(9);
			dirty.session.handle(request::edit_record(dirty.extra, set));
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
		const DialogsView::UnsavedPrompt &prompt = dirty.view().dialogs.unsaved_prompt;
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
		// What waits names its document, the project a switch opens, or the file it renames.
		const std::string named = !request.dir.empty() ? request.dir : request.path;
		TEST_EXPECT(prompt.target == (row.guard == GuardScope::Document ? dirty.extra : named));
	}
	// Export (S16) guards as Build does; a texture's split (S18) and a move (DI-03) as a rename does.
	// DI-25: a delete, a duplicate, a folder's rename and the file history's undo and redo too.
	TEST_EXPECT(prompted == 21 && went_ahead == kEditorRequestKindCount - 21);
	return 0;
}

// A guarded request that touches no unsaved file goes ahead: a Close or a Reload of a clean
// document, a rename of a clean file that names none of the unsaved one's references (planned
// over a graph that holds every edit: with a validation due, a rename's prompt would list every
// document with unsaved edits, S13 A3).
static int test_untouched_goes_ahead() {
	Dirty dirty("opennova_editor_unsaved_guard_clean");
	TEST_EXPECT(dirty.ready());
	dirty.session.handle(request::open_document("main.mnu"));
	const std::string main = dirty.session.document_for("main.mnu")->path();
	dirty.session.handle(request::reload_document(main));
	TEST_EXPECT(!dirty.view().dialogs.unsaved_prompt.open && dirty.session.outcome().done());
	dirty.session.handle(request::close_document(main));
	TEST_EXPECT(!dirty.view().dialogs.unsaved_prompt.open && dirty.session.outcome().done() && !dirty.session.document_for(main));
	dirty.session.run_operations();
	dirty.session.handle(request::rename_asset("menu_style.mns", "renamed.mns"));
	dirty.session.run_operations();
	TEST_EXPECT(!dirty.view().dialogs.unsaved_prompt.open);
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
