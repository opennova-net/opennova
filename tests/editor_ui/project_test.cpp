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
	CHECK(preview->Active && document->Size.x >= kFeedTableRoomEm * ImGui::GetFontSize(),
	      "at 1920 wide the table has its room: the live menu shown beside it (the review's L15d)");
	// Narrower (the audit's 1600, the Document some 330 px), the table takes the centre.
	ImGui::GetIO().DisplaySize = ImVec2(1600.0f, 1080.0f);
	ui.frames(4);
	CHECK(!preview->Active && std::fabs(document->Size.x - centre()) < separators,
	      "at 1600 wide the table lacks its room beside the Preview: it takes the centre");
	// Wider, the Preview keeps the width it had and the Document takes what grew (the centre's split).
	ImGui::GetIO().DisplaySize = ImVec2(3600.0f, 1080.0f);
	ui.frames(4);
	CHECK(preview->Active && document->Size.x >= kFeedTableRoomEm * ImGui::GetFontSize(),
	      "at 3600 wide it has the room: the menu shown beside the table");
	ImGui::GetIO().DisplaySize = ImVec2(1600.0f, 1080.0f);
	ui.frames(4);
	CHECK(!preview->Active, "narrow again: the table first");
	ImGui::GetIO().DisplaySize = ImVec2(1920.0f, 1080.0f);
	ui.frames(4);
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
	const ImGuiID files = Ui::window_id("Files");
	// The Kind column of Files' own table (its id of its own, so no layout saved with it hidden hides it).
	const ImGuiTable *table = ImGui::TableFindByID(item_id(files, {"project_files"}));
	CHECK(table && table->ColumnsCount == 3 && table->Columns[1].IsEnabled, "the Kind column shown");
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
	// The card, Files closed (the review's L14: the card is drawn whether Files draws or not).
	devtools::Window *files_window = nullptr;
	for (int i = 0; i < ui.windows.pass().window_count(); ++i)
		if (std::strcmp(ui.windows.pass().window(i).title(), "Files") == 0) files_window = &ui.windows.pass().window(i);
	if (files_window) files_window->open = false;
	ui.frames(2);
	session.handle(request::about_file("tone.wav"));
	run.settle();
	text = logged_frame(ui);
	CHECK(text.find("About tone.wav") != std::string::npos && text.find("Wave, ") != std::string::npos &&
	              (text.find("Mono, ") != std::string::npos || text.find("Stereo, ") != std::string::npos) &&
	              text.find("Packed into localres.pff.") != std::string::npos && text.find("Named by (1)") != std::string::npos,
	      "the card, Files closed: what it is, its sound, where it goes, who names it");
	if (files_window) files_window->open = true;
	const ImGuiID card = ImHashStr("###file_card");
	CHECK(v.workspace.card.path == "sounds/tone.wav", "the card is the workspace's");
	ui.activate(item_id(card, {"Play##card"}));
	const std::vector<EditorRequest> raised = ui.drain();
	const EditorRequest *play = only(raised, EditorRequestKind::PlaySound);
	CHECK(play && play->path == "sounds/tone.wav", "Play: the wave played");
	if (play) session.handle(*play);
	CHECK(v.workspace.sound.path == "sounds/tone.wav" && v.workspace.sound.state == WorkspaceView::SoundState::Starting,
	      "the sound the session's: starting until the Shell reports it (none here)");
	session.report_sound(v.workspace.sound.serial, WorkspaceView::SoundState::Playing);
	run.settle();
	CHECK(logged_frame(ui).find("Playing") != std::string::npos, "the card says it plays, as the Shell reports");
	// Closed, the card stops what its Play played (the review's L4): its close the workspace's, which stops it.
	ImGuiWindow *card_window = ImGui::FindWindowByName("###file_card");
	if (card_window) ui.activate(card_window->GetID("#CLOSE"));
	ui.session = &session; // the set_workspace it raises served by the session
	const std::vector<EditorRequest> closed = ui.drain();
	CHECK(card_window && closed.empty() && v.workspace.card.path.empty(), "the card closed: the workspace's");
	CHECK(v.workspace.sound.state == WorkspaceView::SoundState::Stopped, "the card closed: the sound stopped");
	// The bank's card: its waves named, the one the project has a Play of its own, the others missing.
	session.handle(request::about_file("menu.lwf"));
	run.settle();
	text = logged_frame(ui);
	CHECK(text.find("About menu.lwf") != std::string::npos && text.find("missing from the project") != std::string::npos,
	      "a sound bank's card lists its waves");
	// Another project: its own Files (no filter, no kind narrowing it), the card of the last one closed (the
	// review's L15c).
	session.handle(request::new_project(dir.file("other"), "Other"));
	run.settle();
	text = logged_frame(ui);
	CHECK(text.find("About menu.lwf") == std::string::npos && text.find("Every kind") != std::string::npos,
	      "another project: the card closed, every kind listed");
}

// The welcome page's install: the editor's last chosen in the field, checked (a CheckInstall raised once,
// not again while the answer stands), what it holds said under it; a folder picked for it checked in its
// turn; one that holds no game holds Create back; Create carries the install. A new project with few files
// offers the game's files: the main menu with what it needs.
void test_welcome_install_and_first_steps() {
	SessionView v;
	// The editor's own install fills the form, never the one in effect (an open project's own, or a session's:
	// the review's L2).
	v.project.editor_install = "C:/Games/JO";
	v.project.retail_directory = "C:/Session/Other";
	Ui ui;
	ui.serve_workspace = false; // the set_workspace requests looked at, the view set by hand
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
	// The view's one slot taken by another field's check (Project settings'): this field keeps its own answer
	// (the review's L12).
	const InstallCheck answered = v.project.install_check;
	v.project.install_check = InstallCheck();
	v.project.install_check.root = absolute_install_path("C:/Elsewhere");
	v.revisions.touch(ViewConcern::Preferences);
	ui.frames(2);
	CHECK(logged_frame(ui).find(": 9,290 files") != std::string::npos && only(ui.drain(), EditorRequestKind::CheckInstall) == nullptr,
	      "another field's check over the slot: this field's line as it was, nothing asked again");
	v.project.install_check = answered;
	// The field cleared: what a project made then takes, the editor's install (the review's L1).
	const ImGuiID welcome = item_id(Ui::window_id("Document"), {"welcome"});
	ImGui::ActivateItemByID(item_id(welcome, {"Game install"}));
	GImGui->NavNextActivateFlags = ImGuiActivateFlags_PreferInput;
	ui.frames(2);
	ui.key(ImGuiMod_Ctrl, true);
	ui.key(ImGuiKey_A, true);
	ui.key(ImGuiKey_A, false);
	ui.key(ImGuiMod_Ctrl, false);
	ui.key(ImGuiKey_Backspace, true);
	ui.key(ImGuiKey_Backspace, false);
	ImGui::ClearActiveID();
	ui.frames(2);
	CHECK(logged_frame(ui).find("Left empty, the project takes the editor's game install, C:/Games/JO.") != std::string::npos,
	      "the field cleared: the install a project takes then said");
	// A refused New project or Open says why on the page (the review's M3).
	v.project.refused = "The project could not be created: C:/mods/New holds a project already.";
	v.revisions.touch(ViewConcern::Project);
	ui.frames(2);
	CHECK(logged_frame(ui).find("C:/mods/New holds a project already.") != std::string::npos, "the refusal said on the page");
	v.project.refused.clear();
	// A folder picked: checked; it holds no game: Create held back. The picks fill the form, the workspace's
	// (the MCP gaps lane: set_workspace, which the session takes, here the view it would make).
	ui.windows.deliver_pick(PickPurpose::NewProjectLocation, "C:/mods/New");
	ui.windows.deliver_pick(PickPurpose::NewProjectInstall, "C:/Empty");
	raised = ui.drain();
	CHECK(workspace_sets(raised, "new_project", "dir").size() == 1 &&
	              !workspace_sets(raised, "new_project", "game_install").empty() &&
	              workspace_sets(raised, "new_project", "game_install").back().string == "C:/Empty",
	      "the picks fill the form, the workspace's");
	v.workspace.new_project.dir = "C:/mods/New";
	v.workspace.new_project.game_install = "C:/Empty";
	v.workspace.new_project.install_named = true;
	v.revisions.touch(ViewConcern::Workspace);
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
	ui.activate(item_id(welcome, {"Create project"}));
	CHECK(only(ui.drain(), EditorRequestKind::NewProject) == nullptr, "Create held back");
	ui.windows.deliver_pick(PickPurpose::NewProjectInstall, "C:/Games/JO");
	v.workspace.new_project.game_install = "C:/Games/JO";
	v.revisions.touch(ViewConcern::Workspace);
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
	// The windows back from standing aside take no keyboard (the review's L15a).
	const ImGuiWindow *focused = GImGui->NavWindow ? GImGui->NavWindow->RootWindow : nullptr;
	const std::string focused_name = focused ? focused->Name : "";
	CHECK(focused_name.rfind("Files", 0) != 0 && focused_name.rfind("Inspector", 0) != 0 &&
	              focused_name.rfind("Problems", 0) != 0 && focused_name.rfind("Output", 0) != 0,
	      "a project open over the welcome page: the windows coming back take no keyboard");
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
