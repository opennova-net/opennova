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
#include <editor/project/local_settings.h>
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

// Files: the Kind column shown; "texture" typed into the filter lists the textures, the one at the root
// whose name does not hold the word among them, and no wave; the kind list narrows to the waves; an
// AboutFile (a double click on a file the editor opens nothing of raises the same card) opens the card
// over the editor: what the wave is, how it sounds as the game decodes it, Play raising play_sound, who
// names it; Close.
void test_files_kind_and_card() {
	editor_test::TempProjectDir dir("opennova_editor_ui_files_card");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	CHECK(session.handle(request::new_project(dir.file("project"), "Sounds")), "a project");
	session.run_operations();
	const SessionView &v = session.view();
	const std::string root = v.project.root;
	const std::string fixtures = std::string(test_paths_repo_root(__FILE__)) + "/fixtures/lwf/";
	CHECK(editor_test::write_bytes(root + "/sounds/menu.lwf", test_io::read_file(fixtures + "menu.lwf")) &&
	              editor_test::write_bytes(root + "/sounds/tone.wav", test_io::read_file(fixtures + "tone.wav")) &&
	              editor_test::write_text(root + "/rootpic.tga", "not a picture") &&
	              editor_test::write_text(root + "/textures/aa.tga", "not a picture"),
	      "the files");
	session.handle(request::rescan());
	session.run_operations();
	DrawnDevices devices;
	Ui ui;
	ui.windows.set_view(&v);
	ui.windows.set_devices(&devices.cache);
	Run run{ session, devices, ui };
	run.settle();
	std::string text = logged_frame(ui);
	CHECK(text.find("Kind") != std::string::npos && text.find("Sound bank") != std::string::npos, "the Kind column shown");
	const ImGuiID files = Ui::window_id("Files");
	ImGui::ActivateItemByID(item_id(files, {"##filter"}));
	GImGui->NavNextActivateFlags = ImGuiActivateFlags_PreferInput;
	ui.frames(2);
	ImGui::GetIO().AddInputCharactersUTF8("texture");
	ui.frames(2);
	ImGui::ClearActiveID();
	ui.frames(1);
	text = logged_frame(ui);
	CHECK(text.find("rootpic.tga") != std::string::npos && text.find("aa.tga") != std::string::npos &&
	              text.find("tone.wav") == std::string::npos && text.find("2 of ") != std::string::npos,
	      "\"texture\": the textures, the one at the root by its kind, no wave");
	// The text cleared, the kind list: waves alone.
	ImGui::ActivateItemByID(item_id(files, {"##filter"}));
	GImGui->NavNextActivateFlags = ImGuiActivateFlags_PreferInput;
	ui.frames(2);
	ui.key(ImGuiMod_Ctrl, true);
	ui.key(ImGuiKey_A, true);
	ui.key(ImGuiKey_A, false);
	ui.key(ImGuiMod_Ctrl, false);
	ui.key(ImGuiKey_Backspace, true);
	ui.key(ImGuiKey_Backspace, false);
	ImGui::ClearActiveID();
	ui.frames(1);
	ui.activate(item_id(files, {"##kind"}));
	ui.activate(item_id(ImHashStr("##Combo_00"), {"Wave (1)###wave"}));
	// Files draws first: its text runs to the Document window's.
	text = logged_frame(ui);
	text = text.substr(0, text.find("Double-click a file in Files"));
	CHECK(text.find("tone.wav") != std::string::npos && text.find("rootpic.tga") == std::string::npos &&
	              text.find("menu.lwf") == std::string::npos && text.find("1 of 4 files") != std::string::npos,
	      "Wave: the waves alone");
	ImGui::ClosePopupsExceptModals();
	// The card.
	session.handle(request::about_file("tone.wav"));
	run.settle();
	text = logged_frame(ui);
	CHECK(text.find("About tone.wav") != std::string::npos && text.find("Wave, ") != std::string::npos &&
	              (text.find("Mono, ") != std::string::npos || text.find("Stereo, ") != std::string::npos) &&
	              text.find("Packed into localres.pff.") != std::string::npos && text.find("Named by (1)") != std::string::npos,
	      "the card: what it is, its sound, where it goes, who names it");
	const ImGuiID card = ImHashStr("###file_card");
	ui.activate(item_id(card, {"Play##card"}));
	const std::vector<EditorRequest> raised = ui.drain();
	const EditorRequest *play = only(raised, EditorRequestKind::PlaySound);
	CHECK(play && play->path == "sounds/tone.wav", "Play: the Shell plays the wave");
	// The bank's card: its waves named, the one the project has a Play of its own, the others missing.
	session.handle(request::about_file("menu.lwf"));
	run.settle();
	text = logged_frame(ui);
	CHECK(text.find("About menu.lwf") != std::string::npos && text.find("missing from the project") != std::string::npos,
	      "a sound bank's card lists its waves");
}

// The welcome page's install: the editor's last chosen in the field, checked (a CheckInstall raised once,
// not again while the answer stands), what it holds said under it; a folder picked for it checked in its
// turn; one that holds no game holds Create back; Create carries the install. A new project with few files
// offers the game's files: the main menu with what it needs.
void test_welcome_install_and_first_steps() {
	SessionView v;
	v.project.retail_directory = "C:/Games/JO";
	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(4);
	std::vector<EditorRequest> raised = ui.drain();
	const EditorRequest *check = only(raised, EditorRequestKind::CheckInstall);
	CHECK(check && check->game_install == "C:/Games/JO", "the editor's install checked");
	CHECK(logged_frame(ui).find("Checking the folder...") != std::string::npos, "until it is answered: checking");
	v.project.install_check.root = absolute_install_path("C:/Games/JO");
	v.project.install_check.game = "jo";
	v.project.install_check.exists = v.project.install_check.mounts = v.project.install_check.executable = true;
	v.project.install_check.files = 9290;
	v.project.install_check.expansions = {{"jox01", "Escalation"}};
	v.revisions.touch(ViewConcern::Preferences);
	ui.frames(2);
	std::string text = logged_frame(ui);
	CHECK(text.find(": 9,290 files, the expansion Escalation (jox01).") != std::string::npos, "what it holds");
	CHECK(only(ui.drain(), EditorRequestKind::CheckInstall) == nullptr, "asked once");
	// A folder picked: checked; it holds no game: Create held back.
	ui.windows.deliver_pick(PickPurpose::NewProjectLocation, "C:/mods/New");
	ui.windows.deliver_pick(PickPurpose::NewProjectInstall, "C:/Empty");
	ui.frames(2);
	raised = ui.drain();
	check = only(raised, EditorRequestKind::CheckInstall);
	CHECK(check && check->game_install == "C:/Empty", "the folder picked checked");
	v.project.install_check = InstallCheck();
	v.project.install_check.root = absolute_install_path("C:/Empty");
	v.project.install_check.exists = true;
	v.revisions.touch(ViewConcern::Preferences);
	ui.frames(2);
	CHECK(logged_frame(ui).find("No game here") != std::string::npos, "no game here, said");
	const ImGuiID welcome = item_id(Ui::window_id("Document"), {"welcome"});
	ui.activate(item_id(welcome, {"Create project"}));
	CHECK(only(ui.drain(), EditorRequestKind::NewProject) == nullptr, "Create held back");
	ui.windows.deliver_pick(PickPurpose::NewProjectInstall, "C:/Games/JO");
	v.project.install_check.root = absolute_install_path("C:/Games/JO");
	v.project.install_check.mounts = true;
	v.revisions.touch(ViewConcern::Preferences);
	ui.frames(2);
	ui.drain();
	ui.activate(item_id(welcome, {"Create project"}));
	const std::vector<EditorRequest> creating = ui.drain();
	const EditorRequest *made = only(creating, EditorRequestKind::NewProject);
	CHECK(made && made->dir == "C:/mods/New" && made->game_install == "C:/Games/JO", "Create: with the install");
	// A project open with nothing in it, few files: the game's files offered.
	v.project.open = true;
	v.project.root = "C:/mods/New";
	for (size_t concern = 0; concern < kViewConcernCount; ++concern) v.revisions.touch(static_cast<ViewConcern>(concern));
	ui.frames(3);
	ui.drain();
	text = logged_frame(ui);
	CHECK(text.find("Bring in the game's files") != std::string::npos, "the first steps");
	ui.activate(item_id(Ui::window_id("Document"), {"The main menu and what it needs..."}));
	const std::vector<EditorRequest> planning = ui.drain();
	const EditorRequest *menu = only(planning, EditorRequestKind::PreviewInstallImport);
	CHECK(menu && menu->names == std::vector<std::string>({"main.mnu"}) && menu->with_dependencies,
	      "the main menu with what it needs, planned");
}

} // namespace

void run_project_tests() {
	test_preview_room();
	test_inspector_filter_per_document();
	test_files_kind_and_card();
	test_welcome_install_and_first_steps();
}

} // namespace editor_ui_test
