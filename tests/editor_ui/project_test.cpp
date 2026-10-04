// The project, the import, Files and the layout as a modder meets them (the UX round's project lane), on
// the null backend over a real session: the Preview stepping aside for a document it has nothing of to
// show and for a table that needs the room, and the Inspector's filter kept per document.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "editor_ui_test_support.h"

#include <editor/assets/asset_kinds.h>
#include <editor/assets/asset_registry.h>
#include <editor/ui/preview_window.h>

namespace editor_ui_test {

namespace {

// A session behind the windows, its requests served as the Shell serves them.
struct Run {
	ProjectSession &session;
	DrawnDevices &devices;
	Ui &ui;
	void pump() {
		for (const EditorRequest &request : ui.drain()) session.handle(request);
		session.run_operations();
		devices.sync(session.viewports(), session.view());
	}
	void settle() {
		for (int i = 0; i < 3; ++i) {
			pump();
			ui.frames(1);
		}
		pump();
	}
	void open(const std::string &path) {
		session.handle(request::open_document(path));
		settle();
	}
};

// The first file of `kind` the project has ("" for none).
std::string first_of(const SessionView &v, AssetKind kind) {
	for (const AssetEntry &entry : v.project.scan->entries)
		if (entry.kind == kind) return entry.relative_path;
	return std::string();
}

// The width between Files and the Inspector: the centre the Document and the Preview share.
float centre() {
	const ImGuiWindow *files = ImGui::FindWindowByName("Files");
	const ImGuiWindow *inspector = ImGui::FindWindowByName("Inspector");
	return files && inspector ? inspector->Pos.x - (files->Pos.x + files->Size.x) : 0.0f;
}

// The Preview steps aside for what it has nothing of to show (a definition table beside an open menu) and
// for a table that feeds its picture (a string table) while the Document beside it lacks the table's room
// (kFeedTableRoomEm): at 1920 wide the default split gives the Document 394 pixels, so the table takes the
// centre; at 3600 it has the room, and the Preview shows the menu beside it; with no menu open the table has
// nothing to preview. A menu, which the Preview shows, keeps it.
void test_preview_room() {
	editor_test::TempProjectDir dir("opennova_editor_ui_preview_room");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	CHECK(preview_project(session, dir), "the preview project");
	const SessionView &v = session.view();
	DrawnDevices devices;
	session.viewports().set_devices(&devices.cache);
	Ui ui;
	ui.windows.set_view(&v);
	ui.windows.set_devices(&devices.cache);
	Run run{ session, devices, ui };
	run.settle();
	const std::string strings = first_of(v, AssetKind::Strings);
	CHECK(!strings.empty() && v.project.scan->find("main.mnu") && v.project.scan->find("items.def"),
	      "a menu, a string table and a definition table");
	ImGuiWindow *preview = ImGui::FindWindowByName("Preview");
	ImGuiWindow *document = ImGui::FindWindowByName("Document");
	if (!preview || !document) {
		CHECK(false, "the two windows");
		return;
	}
	const float separators = 2.0f * ImGui::GetStyle().DockingSeparatorSize + 1.0f;
	run.open("main.mnu");
	ui.frames(3);
	CHECK(!preview_stands_aside(v) && preview->Active && document->Size.x < centre() * 0.5f,
	      "a menu: the Preview beside it, the Document its share");
	run.open("items.def");
	ui.frames(3);
	CHECK(preview_stands_aside(v) && !preview->Active && std::fabs(document->Size.x - centre()) < separators,
	      "a definition table beside an open menu: the Preview steps aside, the table the whole centre");
	run.open(strings);
	ui.frames(3);
	CHECK(!preview_stands_aside(v) && preview_feeds_table(v), "a string table feeds the menu shown");
	CHECK(!preview->Active && std::fabs(document->Size.x - centre()) < separators,
	      "at 1920 wide the table lacks its room beside the Preview: it takes the centre");
	// Wider, the Preview keeps the width it had and the Document takes what grew (the centre's split).
	ImGui::GetIO().DisplaySize = ImVec2(3600.0f, 1080.0f);
	ui.frames(4);
	CHECK(preview->Active && document->Size.x >= kFeedTableRoomEm * ImGui::GetFontSize(),
	      "at 3600 wide it has the room: the menu shown beside the table");
	ImGui::GetIO().DisplaySize = ImVec2(1920.0f, 1080.0f);
	ui.frames(4);
	CHECK(!preview->Active, "narrow again: the table first");
	const DocumentBase *menu = session.document_base_for("main.mnu");
	if (menu) session.handle(request::close_document(menu->path()));
	run.open(strings);
	ui.frames(3);
	CHECK(preview_stands_aside(v) && !preview->Active, "no menu open: the table has nothing to preview");
}

// The Inspector's filter is the document's: text typed over a menu's screen that matches none of its fields
// says so with Clear; another document's record is not filtered by it; the menu's comes back with it; Clear
// empties it. A closed document's filter goes with it.
void test_inspector_filter_per_document() {
	editor_test::TempProjectDir dir("opennova_editor_ui_inspector_filter");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	CHECK(preview_project(session, dir), "the preview project");
	const SessionView &v = session.view();
	DrawnDevices devices;
	session.viewports().set_devices(&devices.cache);
	Ui ui;
	ui.windows.set_view(&v);
	ui.windows.set_devices(&devices.cache);
	Run run{ session, devices, ui };
	run.settle();
	run.open("main.mnu");
	const Document *menu = session.document_for("main.mnu");
	if (!menu || menu->rows().empty()) {
		CHECK(false, "the menu's screen");
		return;
	}
	session.handle(request::select_record(menu->path(), { menu->rows().front()->id, menu->rows().front()->kind, 0 }));
	run.settle();
	const ImGuiID inspector = Ui::window_id("Inspector");
	ImGui::ActivateItemByID(item_id(inspector, {"##filter"}));
	GImGui->NavNextActivateFlags = ImGuiActivateFlags_PreferInput;
	ui.frames(2);
	CHECK(ImGui::GetIO().WantTextInput, "the Inspector's filter has the keyboard");
	ImGui::GetIO().AddInputCharactersUTF8("zzqq");
	ui.frames(2);
	ImGui::ClearActiveID();
	ui.frames(1);
	CHECK(logged_frame(ui).find("No field or list matches \"zzqq\".") != std::string::npos,
	      "nothing matches: said with the text, and Clear");
	run.open("items.def");
	const Document *items = session.document_for("items.def");
	if (!items || items->rows().empty()) {
		CHECK(false, "an item");
		return;
	}
	session.handle(request::select_record(items->path(), { items->rows().front()->id, items->rows().front()->kind, 0 }));
	run.settle();
	CHECK(logged_frame(ui).find("zzqq") == std::string::npos, "the item's fields are not filtered by the menu's text");
	run.open("main.mnu");
	CHECK(logged_frame(ui).find("No field or list matches \"zzqq\".") != std::string::npos, "the menu's filter kept");
	ui.activate(item_id(inspector, {"Clear##nothing"}));
	CHECK(logged_frame(ui).find("zzqq") == std::string::npos, "Clear: every field again");
}

} // namespace

void run_project_tests() {
	test_preview_room();
	test_inspector_filter_per_document();
}

} // namespace editor_ui_test
