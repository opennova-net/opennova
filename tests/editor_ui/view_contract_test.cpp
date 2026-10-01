// S13 V3 (ADR 0046 S13, "Adding a document type"): every document type's view (ui/document_views)
// holds to one contract over a file of its type, drawn alone on the null ImGui backend. Each
// DocumentTypeId past None has its row, and a file here (a type with none fails); the view the row
// makes (make_view) is an outline in the mode its row names (its model's), or a view of its own
// where the row names none, and a view that draws its records has no main viewport. With the
// document open and active in a view of the session and its first record selected, the view drawn
// in a window of a Document tab's size, 5 frames at each of two widths (an outline's file-wide
// values opened through its model after the third: an item table's vehicle spawn registry): it
// raises no request, nothing it draws runs past what shows of the window unless it scrolls sideways
// (the bounds sweep's measure: every window's content within its width, every table cell within
// its column), and it draws its document (the title of its first row of its own kind shows, the
// first kind the type declares: a stylesheet's variable, not its comment). A RevealRecord it
// is sent is held until it draws and taken as it does; a MainViewport row's type is one a Main-role
// viewport kind shows (S13 V5). Two open documents of a type get a view each, whose filters are their
// own. A string table's cell being edited keeps the keyboard, and ends its edit, scrolled out of
// sight. S13 D9: every text type's view draws its lines (its first line that is not blank shows),
// has no main viewport, and a RevealText it is sent marks its line; a text of many thousand lines
// draws only those in sight, the line a reveal names among them. The MainViewport role's view (no
// type plays it yet) draws its outline beside the viewport, whose canvas fills the rest of the tab
// through the workspace's device; in the Document window while an operation holds the documents,
// its outline is held back and its canvas is not (a drag orbits its camera).
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <editor/documents/def_catalog_document.h>
#include <editor/documents/document_types.h>
#include <editor/documents/model_document.h>
#include <editor/documents/strings_document.h>
#include <editor/model/text_document.h>
#include <editor/preview/model_viewport.h>
#include <editor/preview/viewport_kinds.h>
#include <editor/session/session_operation.h>
#include <editor/ui/document_views.h>
#include <editor/ui/document_window.h>
#include <editor/ui/main_viewport_view.h>
#include <editor/ui/text_view.h>
#include <formats/rtxt/rtxt.h>
#include "editor_ui_test_support.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace editor_ui_test {

namespace {

// A workspace of the test's own: a view it seeds, the requests it is asked, and the devices a test
// hands it (none unless it does).
class TestWorkspace : public Workspace {
public:
	SessionView seeded;
	std::vector<EditorRequest> requests;
	ViewportDeviceSource *source = nullptr;
	const SessionView &view() const override { return seeded; }
	void request(EditorRequest request) override { requests.push_back(std::move(request)); }
	ViewportDeviceSource *devices() const override { return source; }
};

struct Fixture {
	AssetKind kind;
	std::string name;
	std::vector<uint8_t> bytes;
};

std::vector<uint8_t> text_bytes(const std::string &text) { return std::vector<uint8_t>(text.begin(), text.end()); }

// A file of each type's kinds: the repo's fixtures, and the three catalogs written here (an item
// table with a vehicle spawn registry, a weapon table with an action and a carry limit, an ammo
// table with an effect).
std::vector<Fixture> fixtures(const std::string &repo) {
	const auto file = [&](const char *relative) { return test_io::read_file(repo + "/fixtures/" + relative); };
	return {
	        {AssetKind::ItemDefs, "items.def",
	         text_bytes("begin \"Drivable Dune Buggy With A Name Long Enough To Be Cut In A Narrow Tab\"\nid 101291\n"
	                    "type vehicle\ngraphic Dbuggy1\npcvehicle_spawnlist 8 12\nend\n"
	                    "begin \"Marker Alpha\"\nid 100001\ntype marker\nend\n")},
	        {AssetKind::WeaponDefs, "weapon.def",
	         text_bytes("weapon \"WPN_CONTRACT\"\ncategory 11\nclipsize 30\naction \"FIRE\"\ndelayend 2\nend\nend\n"
	                    "ammoclass_max_carry CLASS_AK47 300\n")},
	        {AssetKind::AmmoDefs, "ammo.def", text_bytes("ammo AT_CONTRACT\nmax_age 1.5\nvelocity 900\nend\n")},
	        {AssetKind::Strings, "synth_game.bin", file("rtxt/synth_game.bin")},
	        {AssetKind::Menu, "all_widgets.mnu", file("mnu/all_widgets.mnu")},
	        {AssetKind::MenuStyle, "test_style.mns", file("mns/test_style.mns")},
	        {AssetKind::Model, "armory.3di", file("threedi/synth/armory.3di")},
	        {AssetKind::Animation, "walk.bad", file("anim/walk.bad")},
	        {AssetKind::AnimationMap, "soldier.adm", file("anim/soldier.adm")},
	        // The text types (S13 D9): a script, a music script, a credits file (whose lines its text
	        // form cannot carry: held read only, drawn all the same), a plain shader and a
	        // configuration.
	        {AssetKind::Script, "text_document.wac", file("wac/text_document.wac")},
	        {AssetKind::MusicScript, "gamemus.bin", file("mus/synth_gamemus.bin")},
	        {AssetKind::Credits, "nlist.kda", file("cbin/synth_nlist.kda")},
	        {AssetKind::Shader, "glass.fx", text_bytes("// glass\r\nfloat4 main() : COLOR { return 0; }\r\n")},
	        {AssetKind::Config, "game.cfg", text_bytes("\r\n[Game]\r\nname = Views\r\n")},
	};
}

// The view of an open document as the session shows it: the project, the file in its scan, the
// document open and active, its first row selected.
void seed(SessionView &view, const std::shared_ptr<const DocumentBase> &document) {
	view.project.open = true;
	view.project.root = "C:/mods/Views";
	editor_test::own(view.project.document).title = "Views";
	editor_test::own(view.project.scan).entries = {file_entry(document->path(), document->path(), document->kind())};
	editor_test::own(view.project.scan).index();
	view.documents.open = {document};
	view.documents.active = document->path();
	if (const Document *records = records_of(*document); records && !records->rows().empty()) {
		const NodeAddress first{records->rows()[0]->id, records->rows()[0]->kind, 0};
		view.documents.selection.select_only(document->path(), first);
	}
	for (size_t concern = 0; concern < kViewConcernCount; ++concern)
		view.revisions.touch(static_cast<ViewConcern>(concern));
}

// A frame of the view drawn alone in a window `width` pixels wide and 560 high, a Document tab's
// room, its modals after it as the workspace draws them; what it wrote as text when `log`.
std::string view_frame(TestWorkspace &workspace, DocumentView &view, const DocumentBase &document, float width,
                       bool log = false) {
	ImGui::NewFrame();
	ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
	ImGui::SetNextWindowSize(ImVec2(width, 560.0f));
	ImGui::Begin("Tab", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
	if (log) ImGui::LogToBuffer();
	view.draw(workspace, document);
	std::string text;
	if (log) {
		text = GImGui->LogBuffer.c_str();
		ImGui::LogFinish();
	}
	ImGui::End();
	view.draw_modals(workspace);
	ImGui::Render();
	return text;
}

void draw_frames(TestWorkspace &workspace, DocumentView &view, const DocumentBase &document, float width,
                 int count) {
	for (int i = 0; i < count; ++i) view_frame(workspace, view, document, width);
}

// The title of the document's first row of its first kind (the type's own records: a catalog's
// items, a stylesheet's variables rather than its comments): what its view shows first; a text's
// first line that is not blank.
std::string first_title(const DocumentBase &document) {
	if (const TextDocument *text = text_of(document)) {
		for (size_t line = 1; line <= text->line_count(); ++line)
			if (text->line(line).find_first_not_of(" \t") != std::string_view::npos) return std::string(text->line(line));
		return std::string();
	}
	const Document *records = records_of(document);
	if (!records || records->kinds().empty()) return std::string();
	const NodeKind own = records->kinds().front().kind;
	for (const auto &row : records->rows())
		if (row && row->kind == own) return records->record_title({row->id, row->kind, 0});
	return std::string();
}

void test_every_view() {
	const std::string repo = test_paths_repo_root(__FILE__);
	const std::vector<Fixture> files = fixtures(repo);
	size_t types = 0, views = 0, frames = 0;
	for (size_t id = 1; id <= kDocumentTypeCount; ++id) {
		const DocumentTypeId type_id = static_cast<DocumentTypeId>(id);
		const DocumentType *type = document_type(type_id);
		const DocumentViewRow *row = document_view_row(type_id);
		CHECK(type && row && row->type == type_id && (row->outline || row->make),
		      "every document type has its view's row, an outline or a make");
		if (!type || !row) continue;
		// A MainViewport row's type is one a Main-role viewport kind shows, the tab's viewport, with
		// the outline beside it.
		if (row->role == DocumentViewRole::MainViewport) {
			const ViewportKind main = main_viewport_kind(type_id);
			CHECK(main != ViewportKind::kCount && viewport_kind_row(main).role == ViewportRole::Main && row->outline,
			      (std::string(type->name) + ": its MainViewport row's Main-role kind and outline").c_str());
		}
		size_t drawn = 0;
		for (const Fixture &fixture : files) {
			if (asset_kind_row(fixture.kind).document != type_id) continue;
			++drawn;
			const std::string where = std::string(type->name) + " over " + fixture.name;
			std::shared_ptr<DocumentBase> loaded = type->make();
			Diagnostic error;
			CHECK(loaded && loaded->load_bytes(fixture.bytes, fixture.name, fixture.kind, "jo", error),
			      (where + ": the file loads").c_str());
			if (!loaded) continue;
			const std::shared_ptr<const DocumentBase> document = loaded;
			std::unique_ptr<DocumentView> view = make_view(*document);
			CHECK(view != nullptr, (where + ": make_view makes its view").c_str());
			if (!view) continue;
			// An outline in its row's mode, or a view of its own where the row names no outline.
			OutlineModel *outline = view->outline();
			CHECK(row->outline ? outline && outline->mode() == row->outline->mode : outline == nullptr,
			      (where + ": the view is the outline its row names, in its mode, or its own").c_str());
			NullBackend backend;
			TestWorkspace workspace;
			seed(workspace.seeded, document);
			++views;
			for (const float width : {520.0f, 320.0f}) {
				const std::string at = where + " at " + std::to_string(int(width));
				draw_frames(workspace, *view, *document, width, 3);
				if (outline) outline->set_values_open(true);
				draw_frames(workspace, *view, *document, width, 2);
				frames += 5;
				CHECK(workspace.requests.empty(), (at + ": drawing raises no request").c_str());
				for (const std::string &offender : overflowing()) CHECK(false, (at + ": " + offender).c_str());
			}
			// It draws its document: the title of its first row of its own kind shows.
			const std::string title = first_title(*document);
			const std::string shown = view_frame(workspace, *view, *document, 520.0f, true);
			CHECK(!title.empty() && shown.find(title.substr(0, 8)) != std::string::npos,
			      (where + ": the view draws its document (" + title + ")").c_str());
			// A view that draws its records or its text has no main viewport; the frame it is asked in
			// draws nothing.
			if (row->role != DocumentViewRole::MainViewport) {
				ImGui::NewFrame();
				CHECK(!view->main_viewport(workspace, *document), (where + ": no main viewport").c_str());
				ImGui::Render();
			}
			// A RevealRecord held until the view draws, taken as it draws.
			ViewEvent reveal;
			reveal.kind = ViewEventKind::RevealRecord;
			reveal.path = document->path();
			reveal.address = workspace.seeded.documents.selection.primary;
			view->receive(reveal);
			CHECK(view->held_events() == 1, (where + ": a RevealRecord held").c_str());
			draw_frames(workspace, *view, *document, 520.0f, 1);
			CHECK(view->held_events() == 0 && workspace.requests.empty(),
			      (where + ": taken as the view draws, nothing raised").c_str());
			// A text's view: a RevealText marks its line as it is taken.
			if (row->role == DocumentViewRole::Text) {
				auto *text_view = dynamic_cast<TextView *>(view.get());
				CHECK(text_view != nullptr, (where + ": a text type's view is the text view").c_str());
				ViewEvent go;
				go.kind = ViewEventKind::RevealText;
				go.path = document->path();
				go.locator = "2:1";
				view->receive(go);
				draw_frames(workspace, *view, *document, 520.0f, 1);
				CHECK(text_view && text_view->marked_line() == 2 && view->held_events() == 0,
				      (where + ": a RevealText marks its line").c_str());
			}
		}
		CHECK(drawn > 0, (std::string(type->name) + ": the contract has a file of its type").c_str());
		types += drawn > 0 ? 1 : 0;
	}
	CHECK(types == kDocumentTypeCount, "every document type's view drawn");
	std::printf("%zu document types, %zu views over their files, %zu frames drawn\n", types, views, frames);
}

// Two open catalogs in the Document window: a view each, whose filter (its model's) is its own (the
// one view a type shared before S13 V3 filtered every open catalog by the text typed into one).
void test_a_view_per_document() {
	editor_test::TempProjectDir dir("opennova_editor_view_per_document");
	const auto load_catalog = [&](const char *file, const char *relative, const char *text) {
		auto document = std::make_shared<DefCatalogDocument>();
		Diagnostic error;
		CHECK(editor_test::write_text(dir.file(file), text), "catalog fixture");
		CHECK(document->load(dir.file(file), relative, AssetKind::ItemDefs, "jo", error), error.message.c_str());
		return document;
	};
	const auto a = load_catalog("a.def", "a/items.def", "begin \"Alpha\"\nid 100001\ntype marker\nend\n");
	const auto b = load_catalog("b.def", "b/items.def", "begin \"Bravo\"\nid 100002\ntype marker\nend\n");
	SessionView v;
	v.project.open = true;
	v.project.root = dir.root();
	editor_test::own(v.project.document).title = "Views";
	editor_test::own(v.project.scan).entries = {file_entry("items.def", a->path(), AssetKind::ItemDefs),
	                                          file_entry("items.def", b->path(), AssetKind::ItemDefs)};
	editor_test::own(v.project.scan).index();
	v.documents.open = {a, b};
	v.documents.active = a->path();
	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.focus("Document");
	ui.away();
	ui.drain();
	DocumentWindow *documents = nullptr;
	for (int i = 0; i < ui.windows.pass().window_count() && !documents; ++i)
		documents = dynamic_cast<DocumentWindow *>(&ui.windows.pass().window(i));
	CHECK(documents != nullptr, "the Document window");
	if (!documents) return;
	CHECK(documents->view_of(a->path()) && !documents->view_of(b->path()), "a view for the document drawn");
	OutlineModel *a_outline = documents->view_of(a->path()) ? documents->view_of(a->path())->outline() : nullptr;
	CHECK(a_outline != nullptr, "a's view is an outline");
	if (!a_outline) return;
	// a filtered: it lists none of its records.
	a_outline->set_filter("zzz");
	ui.frames(2);
	ui.away();
	CHECK(logged_frame(ui).find("No record matches the filter.") != std::string::npos, "a's filter hides its records");
	// b made active: its own view, its own (empty) filter.
	v.documents.active = b->path();
	v.revisions.touch(ViewConcern::ActiveDocument);
	v.revisions.touch(ViewConcern::Selection);
	ui.frames(4);
	ui.away();
	const std::string shown = logged_frame(ui);
	DocumentView *b_view = documents->view_of(b->path());
	CHECK(b_view && b_view != documents->view_of(a->path()) && b_view->outline() && b_view->outline()->filter().empty(),
	      "b gets a view of its own, its filter empty");
	CHECK(shown.find("Bravo") != std::string::npos && shown.find("No record matches the filter.") == std::string::npos,
	      "b's filter is its own: its record listed");
	// Back to a: its filter kept.
	v.documents.active = a->path();
	v.revisions.touch(ViewConcern::ActiveDocument);
	v.revisions.touch(ViewConcern::Selection);
	ui.frames(4);
	ui.away();
	CHECK(a_outline->filter() == "zzz" && logged_frame(ui).find("No record matches the filter.") != std::string::npos,
	      "a keeps its filter");
	// b closed: its view goes with it.
	v.documents.open = {a};
	v.revisions.touch(ViewConcern::DocumentSet);
	ui.frames(2);
	CHECK(documents->view_of(a->path()) && !documents->view_of(b->path()), "a closed document's view goes");
	ui.drain();
}

// A string table's cell being edited, its row scrolled out of sight (the detail table draws only
// the rows in sight): it keeps the keyboard, and Enter ends its edit (an EndEdit for the table),
// where the row's cells not drawn had let the keyboard go with the edit group still open.
void test_edit_scrolled_out() {
	opennova::rtxt::File table;
	table.sections = {{"Menu", 60}};
	for (int i = 0; i < 60; ++i) {
		char key[16];
		std::snprintf(key, sizeof(key), "KEY_%02d", i);
		table.entries.push_back({key, "a text", {}, 0});
	}
	std::vector<uint8_t> bytes;
	std::string io_error;
	CHECK(opennova::rtxt::write(table, bytes, io_error), "the table written");
	auto loaded = std::make_shared<StringsDocument>();
	Diagnostic error;
	CHECK(loaded->load_bytes(bytes, "many.bin", AssetKind::Strings, "jo", error), "the table loads");
	const std::shared_ptr<const DocumentBase> document = loaded;
	std::unique_ptr<DocumentView> view = make_view(*document);
	CHECK(view != nullptr && !loaded->rows().empty(), "its view and its section");
	if (!view || loaded->rows().empty()) return;
	std::vector<NodeId> strings;
	for (const Document::Collection &collection : loaded->collections_of({loaded->rows()[0]->id, loaded->rows()[0]->kind, 0}))
		strings = collection.ids;
	CHECK(strings.size() == 60, "the section's strings");
	if (strings.empty()) return;
	NullBackend backend;
	TestWorkspace workspace;
	seed(workspace.seeded, document); // the section selected: its strings the detail table
	draw_frames(workspace, *view, *document, 520.0f, 3);
	// The first string's key cell given the keyboard, a character typed: its Set.
	const ImGuiID records = item_id(Ui::window_id("Tab"), {"master", "records"});
	const ImGuiID key = item_id(pushed(records, static_cast<int>(strings.front())), {"##key"});
	ImGui::ActivateItemByID(key);
	GImGui->NavNextActivateFlags = ImGuiActivateFlags_PreferInput;
	draw_frames(workspace, *view, *document, 520.0f, 2);
	CHECK(GImGui->ActiveId == key, "the first key's cell has the keyboard");
	ImGui::GetIO().AddInputCharactersUTF8("x");
	draw_frames(workspace, *view, *document, 520.0f, 2);
	CHECK(!workspace.requests.empty() && workspace.requests.back().kind == EditorRequestKind::EditRecord,
	      "typed: the key's Set");
	// The table scrolled to its end: the first string far out of sight.
	ImGuiTable *details = ImGui::TableFindByID(records);
	CHECK(details && details->InnerWindow && details->InnerWindow->ScrollMax.y > 0.0f, "the detail table scrolls");
	if (!details || !details->InnerWindow) return;
	ImGui::SetScrollY(details->InnerWindow, details->InnerWindow->ScrollMax.y);
	draw_frames(workspace, *view, *document, 520.0f, 3);
	CHECK(details->InnerWindow->Scroll.y > 0.0f && GImGui->ActiveId == key,
	      "scrolled out of sight, the cell keeps the keyboard");
	workspace.requests.clear();
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
	draw_frames(workspace, *view, *document, 520.0f, 1);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
	draw_frames(workspace, *view, *document, 520.0f, 1);
	bool ended = false;
	for (const EditorRequest &request : workspace.requests)
		ended = ended || (request.kind == EditorRequestKind::EndEdit && request.path == document->path());
	CHECK(ended && GImGui->ActiveId != key, "Enter ends its edit");
}

// S13 V5: the MainViewport role's view (ui/main_viewport_view), which no type's row plays yet: the
// document's outline in a column, the viewport beside it, its canvas filling the rest of the tab,
// drawn through the workspace's device of (document, kind) once the Shell's pump has made it; the
// viewport the one kept for the document (a model's here, until a Main-role kind ships).
void test_main_viewport_view() {
	const std::string repo = test_paths_repo_root(__FILE__);
	auto model = std::make_shared<ModelDocument>();
	Diagnostic error;
	CHECK(model->load_bytes(test_io::read_file(repo + "/fixtures/threedi/synth/armory.3di"), "armory.3di", AssetKind::Model,
	                        "jo", error),
	      "a model");
	const DocumentViewRow *row = document_view_row(DocumentTypeId::Model);
	CHECK(row && row->outline, "the model's row names an outline");
	if (!row || !row->outline) return;
	NullBackend backend;
	TestWorkspace workspace;
	seed(workspace.seeded, model);
	HandViewports shell;
	shell.bind(workspace.seeded);
	shell.viewports->ensure(model->path(), ViewportKind::Model);
	workspace.source = &shell.devices.cache;
	MainViewportView view(*row->outline, ViewportKind::Model);
	CHECK(view.kind() == ViewportKind::Model && view.outline() && view.outline()->mode() == row->outline->mode,
	      "its outline in its row's mode, beside the kind's viewport");
	// The Shell's pump before each frame: the device the canvas asked for made; the canvas sizes the
	// device as it draws it, raising nothing (the device reports its size at the pump).
	for (int i = 0; i < 4; ++i) {
		shell.devices.sync(*shell.viewports, workspace.seeded);
		view_frame(workspace, view, *model, 720.0f);
		view.end_frame(workspace);
		CHECK(workspace.requests.empty(), "the canvas raises nothing as it draws");
		workspace.requests.clear();
	}
	shell.devices.sync(*shell.viewports, workspace.seeded);
	const ViewportModel *viewport = shell.find(model->path(), ViewportKind::Model);
	const DrawnDevice *device = shell.device(model->path(), ViewportKind::Model);
	CHECK(viewport && viewport->status() == ViewportStatus::Ready, "the viewport shows the model");
	CHECK(device && device->draws > 0 && device->width > 0 && device->width < 720 && device->origin.x > 100.0f,
	      "the viewport drawn on its device beside the outline");
	CHECK(device && viewport && device->width == viewport->size().width && device->height == viewport->size().height &&
	              viewport->canvas_sized(),
	      "the viewport at the canvas's size, its device's as drawn");
	const std::string title = first_title(*model);
	CHECK(!title.empty() && view_frame(workspace, view, *model, 720.0f, true).find(title.substr(0, 8)) != std::string::npos,
	      "the outline drawn beside it");
}

// The MainViewport role's view in the Document window, as its tab draws it, while an operation holds
// the documents (a refresh, S13 A3): its outline column is held back (a click on a line selects
// nothing), its canvas is not (a drag on the picture orbits the camera, a SetViewport that runs
// beside any operation); with no operation, the same click on the line selects its record.
void test_main_viewport_held() {
	const std::string repo = test_paths_repo_root(__FILE__);
	auto model = std::make_shared<ModelDocument>();
	Diagnostic error;
	CHECK(model->load_bytes(test_io::read_file(repo + "/fixtures/threedi/synth/armory.3di"), "armory.3di", AssetKind::Model,
	                        "jo", error),
	      "a model");
	const DocumentViewRow *row = document_view_row(DocumentTypeId::Model);
	CHECK(row && row->outline, "the model's row names an outline");
	if (!row || !row->outline) return;
	SessionView v;
	seed(v, model);
	HandViewports shell;
	shell.bind(v);
	shell.viewports->ensure(model->path(), ViewportKind::Model);
	Ui ui;
	ui.windows.set_view(&v);
	ui.windows.set_devices(&shell.devices.cache);
	ui.pump = [&] { shell.pump(ui.windows, v); };
	ui.frames(2);
	DocumentWindow *documents = nullptr;
	for (int i = 0; i < ui.windows.pass().window_count() && !documents; ++i)
		documents = dynamic_cast<DocumentWindow *>(&ui.windows.pass().window(i));
	CHECK(documents != nullptr, "the Document window");
	if (!documents) return;
	documents->set_view(*model, std::make_unique<MainViewportView>(*row->outline, ViewportKind::Model));
	ui.focus("Document");
	ui.frames(4);
	ui.away();
	ui.drain();
	const auto *viewport = static_cast<const ModelViewport *>(shell.find(model->path(), ViewportKind::Model));
	const DrawnDevice *device = shell.device(model->path(), ViewportKind::Model);
	CHECK(viewport && viewport->status() == ViewportStatus::Ready && device && device->draws > 0,
	      "the viewport drawn in the tab through its device");
	if (!viewport || !device) return;

	// A line of the outline (the tree's own child, in the view's outline column: two outline children
	// deep) under the mouse.
	const ImGuiWindow *window = ImGui::FindWindowByName("Document");
	CHECK(window != nullptr, "the Document window's ImGui window");
	if (!window) return;
	const auto selects = [&](const std::vector<EditorRequest> &requests) {
		return std::count_if(requests.begin(), requests.end(),
		                     [](const EditorRequest &request) { return request.kind == EditorRequestKind::SelectRecord; });
	};
	const float x = window->Pos.x + 40.0f;
	ImVec2 line(0.0f, 0.0f);
	for (float y = window->Pos.y + 30.0f; y < window->Pos.y + window->Size.y && line.y == 0.0f; y += 3.0f) {
		ui.mouse(x, y);
		const ImGuiWindow *hovered = GImGui->HoveredWindow;
		const std::string name = hovered ? hovered->Name : "";
		const size_t first = name.find("/outline_");
		if (GImGui->HoveredId == 0 || first == std::string::npos || name.find("/outline_", first + 1) == std::string::npos)
			continue;
		ui.button(true);
		ui.button(false);
		if (selects(ui.drain()) > 0) line = ImVec2(x, y);
	}
	CHECK(line.y != 0.0f, "with no operation, a click on an outline line selects its record");

	// A refresh holds the documents: the outline held back, the canvas not.
	OperationStatus refresh;
	refresh.id = 9;
	refresh.kind = OperationKind::Refresh;
	refresh.label = "Refreshing";
	refresh.reads = operation_kind_row(OperationKind::Refresh).reads;
	refresh.writes = operation_kind_row(OperationKind::Refresh).writes;
	v.activity.operation = refresh;
	v.revisions.touch(ViewConcern::Operation);
	CHECK(!v.allows(EditorRequestKind::EditRecord) && v.allows(EditorRequestKind::SetViewport),
	      "a refresh holds the documents, never a viewport");
	ui.frames(2);
	ui.drain();
	if (line.y != 0.0f) {
		ui.click(line);
		CHECK(selects(ui.drain()) == 0, "held: a click on the outline line selects nothing");
	}
	ui.away();
	const float yaw = viewport->camera().yaw;
	const ImVec2 from(device->origin.x + 16.0f, device->origin.y + 16.0f);
	ui.mouse(from.x, from.y);
	ui.button(true);
	ui.mouse(from.x + 40.0f, from.y + 8.0f);
	ui.mouse(from.x + 90.0f, from.y + 16.0f);
	ui.button(false);
	ui.frames(2);
	CHECK(viewport->camera().yaw != yaw, "held: a drag on the picture orbits the camera");
	CHECK(selects(ui.drain()) == 0, "the drag selects nothing");
	ui.away();
}

// A text of many lines is a list clipped to what shows (a log of a frame draws every line: Dear
// ImGui's clipper draws all while it logs); a reveal scrolls the line it names into sight; its
// markers are made once per change of the findings or the text, never for a frame.
void test_long_text() {
	std::string text;
	for (int i = 1; i <= 5000; ++i) text += "line " + std::to_string(i) + "\r\n";
	auto loaded = std::make_shared<TextDocument>();
	Diagnostic error;
	CHECK(loaded->load_bytes(text_bytes(text), "long.txt", AssetKind::Text, "jo", error), "the text loads");
	const std::shared_ptr<const DocumentBase> document = loaded;
	std::unique_ptr<DocumentView> view = make_view(*document);
	auto *text_view = dynamic_cast<TextView *>(view.get());
	CHECK(text_view != nullptr, "a text's view");
	if (!text_view) return;
	NullBackend backend;
	TestWorkspace workspace;
	seed(workspace.seeded, document);
	draw_frames(workspace, *view, *document, 520.0f, 3);
	// The lines' child window, scrolled to the top.
	ImGuiWindow *tab = ImGui::FindWindowByName("Tab");
	CHECK(tab != nullptr, "the tab's window");
	if (!tab) return;
	char name[64];
	std::snprintf(name, sizeof(name), "Tab/text_%08X", tab->GetID("text"));
	const ImGuiWindow *lines = ImGui::FindWindowByName(name);
	CHECK(lines != nullptr && lines->Scroll.y == 0.0f, "the lines' window, at the top");
	if (!lines) return;
	const float step = ImGui::GetFrameHeightWithSpacing();
	ViewEvent go;
	go.kind = ViewEventKind::RevealText;
	go.path = document->path();
	go.locator = "4000:3";
	view->receive(go);
	draw_frames(workspace, *view, *document, 520.0f, 3);
	const float top = 3999.0f * step;
	CHECK(text_view->marked_line() == 4000 && lines->Scroll.y <= top && lines->Scroll.y + lines->Size.y >= top + step,
	      "the revealed line scrolled into sight");
	CHECK(text_view->markers_made() == 1 && workspace.requests.empty(), "its markers made once, nothing raised");
	// Clipped: the frame drew the lines in sight (a 560-pixel tab), never the 5,000.
	CHECK(text_view->lines_drawn() > 0 && text_view->lines_drawn() < 100, "the lines in sight drawn alone");
}

} // namespace

} // namespace editor_ui_test

int main() {
	editor_ui_test::test_every_view();
	editor_ui_test::test_long_text();
	editor_ui_test::test_a_view_per_document();
	editor_ui_test::test_edit_scrolled_out();
	editor_ui_test::test_main_viewport_view();
	editor_ui_test::test_main_viewport_held();
	if (editor_ui_test::g_failures) {
		std::printf("%d check(s) failed\n", editor_ui_test::g_failures);
		return 1;
	}
	std::printf("editor_view_contract passed\n");
	return 0;
}
