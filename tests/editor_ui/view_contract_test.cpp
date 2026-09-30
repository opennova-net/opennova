// S13 V3 (ADR 0046 S13, "Adding a document type"): every document type's view (ui/document_views)
// holds to one contract over a file of its type, drawn alone on the null ImGui backend. Each
// DocumentTypeId past None has its row, and a file here (a type with none fails); the view the row
// makes (make_view) plays its row's role, and a view that draws its records has no main viewport.
// With the document open and active in a view of the session and its first record selected, the
// view drawn in a window of a Document tab's size, 3 frames at each of two widths: it raises no
// request, and nothing it draws runs past what shows of the window unless it scrolls sideways (the
// bounds sweep's measure: every window's content within its width, every table cell within its
// column), its sections open (an item table's vehicle spawn registry). A RevealRecord it is sent is
// held until it draws and taken as it does. Two open documents of a type get a view each.
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <editor/documents/def_catalog_document.h>
#include <editor/documents/document_types.h>
#include <editor/ui/document_views.h>
#include <editor/ui/document_window.h>
#include "editor_ui_test_support.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace editor_ui_test {

namespace {

// A workspace of the test's own: a view it seeds, the requests it is asked, no devices.
class TestWorkspace : public Workspace {
public:
	SessionView seeded;
	std::vector<EditorRequest> requests;
	const SessionView &view() const override { return seeded; }
	void request(EditorRequest request) override { requests.push_back(std::move(request)); }
	const WorkspaceDevices &devices() const override { return devices_; }

private:
	WorkspaceDevices devices_;
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
		view.documents.select_only(first);
	}
	for (size_t concern = 0; concern < kViewConcernCount; ++concern)
		view.revisions.touch(static_cast<ViewConcern>(concern));
}

// `count` frames of the view drawn alone in a window `width` pixels wide and 560 high, a
// Document tab's room, its modals after it as the workspace draws them.
void draw_frames(TestWorkspace &workspace, DocumentView &view, const DocumentBase &document, float width,
                 int count) {
	for (int i = 0; i < count; ++i) {
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(width, 560.0f));
		ImGui::Begin("Tab", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
		view.draw(workspace, document);
		ImGui::End();
		view.draw_modals(workspace);
		ImGui::Render();
	}
}

// The section a view folds set open in the window "Tab"'s own state, as a click leaves it: the
// outline's file-wide values (an item table's vehicle spawn registry).
void unfold() {
	ImGuiWindow *tab = ImGui::FindWindowByName("Tab");
	if (!tab) return;
	tab->StateStorage.SetInt(item_id(tab->ID, {"file_values"}), 1);
}

void test_every_view() {
	const std::string repo = test_paths_repo_root(__FILE__);
	const std::vector<Fixture> files = fixtures(repo);
	size_t types = 0, views = 0, frames = 0;
	for (size_t id = 1; id <= kDocumentTypeCount; ++id) {
		const DocumentTypeId type_id = static_cast<DocumentTypeId>(id);
		const DocumentType *type = document_type(type_id);
		const DocumentViewRow *row = document_view_row(type_id);
		CHECK(type && row && row->type == type_id && row->make, "every document type has its view's row");
		if (!type || !row) continue;
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
			CHECK(view->role() == row->role, (where + ": the view plays its row's role").c_str());
			NullBackend backend;
			TestWorkspace workspace;
			seed(workspace.seeded, document);
			++views;
			for (const float width : {520.0f, 320.0f}) {
				const std::string at = where + " at " + std::to_string(int(width));
				draw_frames(workspace, *view, *document, width, 3);
				unfold();
				draw_frames(workspace, *view, *document, width, 2);
				frames += 5;
				CHECK(workspace.requests.empty(), (at + ": drawing raises no request").c_str());
				for (const std::string &offender : overflowing()) CHECK(false, (at + ": " + offender).c_str());
			}
			// A view that draws its records has no main viewport; the frame it is asked in draws nothing.
			if (row->role == DocumentViewRole::Records) {
				ImGui::NewFrame();
				CHECK(!view->main_viewport(workspace, *document), (where + ": no main viewport").c_str());
				ImGui::Render();
			}
			// A RevealRecord held until the view draws, taken as it draws.
			ViewEvent reveal;
			reveal.kind = ViewEventKind::RevealRecord;
			reveal.path = document->path();
			reveal.address = workspace.seeded.documents.selection;
			view->receive(reveal);
			CHECK(view->held_events() == 1, (where + ": a RevealRecord held").c_str());
			draw_frames(workspace, *view, *document, 520.0f, 1);
			CHECK(view->held_events() == 0 && workspace.requests.empty(),
			      (where + ": taken as the view draws, nothing raised").c_str());
		}
		CHECK(drawn > 0, (std::string(type->name) + ": the contract has a file of its type").c_str());
		types += drawn > 0 ? 1 : 0;
	}
	CHECK(types == kDocumentTypeCount, "every document type's view drawn");
	std::printf("%zu document types, %zu views over their files, %zu frames drawn\n", types, views, frames);
}

// Two open catalogs in the Document window: a view each, whose filter is its own (the one view a
// type shared before S13 V3 filtered every open catalog by the text typed into one).
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
	// Typed into a's filter: a lists none of its records.
	ImGui::ActivateItemByID(item_id(document_tab_id(a->path()), {"##filter"}));
	GImGui->NavNextActivateFlags = ImGuiActivateFlags_PreferInput;
	ui.frames(2);
	ImGui::GetIO().AddInputCharactersUTF8("zzz");
	ui.frames(2);
	ImGui::ClearActiveID();
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
	CHECK(documents->view_of(b->path()) && documents->view_of(b->path()) != documents->view_of(a->path()),
	      "b gets a view of its own");
	CHECK(shown.find("Bravo") != std::string::npos && shown.find("No record matches the filter.") == std::string::npos,
	      "b's filter is its own: its record listed");
	// Back to a: its filter kept.
	v.documents.active = a->path();
	v.revisions.touch(ViewConcern::ActiveDocument);
	v.revisions.touch(ViewConcern::Selection);
	ui.frames(4);
	ui.away();
	CHECK(logged_frame(ui).find("No record matches the filter.") != std::string::npos, "a keeps its filter");
	// b closed: its view goes with it.
	v.documents.open = {a};
	v.revisions.touch(ViewConcern::DocumentSet);
	ui.frames(2);
	CHECK(documents->view_of(a->path()) && !documents->view_of(b->path()), "a closed document's view goes");
	ui.drain();
}

} // namespace

} // namespace editor_ui_test

int main() {
	editor_ui_test::test_every_view();
	editor_ui_test::test_a_view_per_document();
	if (editor_ui_test::g_failures) {
		std::printf("%d check(s) failed\n", editor_ui_test::g_failures);
		return 1;
	}
	std::printf("editor_view_contract passed\n");
	return 0;
}
