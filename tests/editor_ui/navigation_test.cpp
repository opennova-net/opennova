// Back and Forward (the navigation history, CONTEXT.md "Navigation history"), on the null backend over a
// real session whose clock the test steps: the menu bar's two arrows, enabled while they go somewhere,
// their tooltips naming where with their keys, a right-click listing the places one goes to; Alt+Left and
// Alt+Right, which a text field with the keyboard keeps; the mouse's back and forward buttons (Dear
// ImGui's buttons 3 and 4, as the bridge maps the OS's); nothing while a dialog takes the whole editor; and
// a Back to a file's page showing its tab over the active document's.
#include <string>
#include <vector>

#include "editor_ui_test_support.h"

#include <editor/assets/asset_kinds.h>
#include <editor/session/navigation_history.h>

namespace editor_ui_test {

namespace {

// A new project with its required files behind the windows; each request served as the Shell serves
// it, a while after the last (never a run's: NavigationHistory::kCoalesceMs).
struct Navigating {
	editor_test::TempProjectDir dir;
	editor_test::FakePlatform platform;
	MemoryPreferencesStore preferences;
	ProjectSession session;
	Ui ui;
	explicit Navigating(const char *name) : dir(name), session(platform, preferences) {
		editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Places"));
		editor_test::create_missing_files(session);
		ui.session = &session;
		ui.windows.set_view(&session.view());
		ui.frames(3);
	}
	void go(const EditorRequest &request) {
		platform.clock += 10 * NavigationHistory::kCoalesceMs;
		session.handle(request);
		session.run_operations();
		ui.frames(2);
	}
	// The windows' requests, each served.
	std::vector<EditorRequest> serve() {
		std::vector<EditorRequest> requests = ui.drain();
		for (const EditorRequest &request : requests) go(request);
		return requests;
	}
	const SessionView &view() const { return session.view(); }
};

ImGuiID arrow(bool back) { return item_id(menu_bar_id(), {back ? "navigate_back" : "navigate_forward", "##go"}); }

// The pointer moved along the main menu bar onto the item `id` (it stays there); false when no point of
// the bar's first quarter hovers it.
bool hover(Ui &ui, ImGuiID id) {
	const ImGuiWindow *bar = ImGui::FindWindowByName("##MainMenuBar");
	if (!bar) return false;
	const float y = bar->Pos.y + bar->Size.y * 0.5f;
	for (float x = bar->Pos.x + 1.0f; x < bar->Pos.x + bar->Size.x * 0.25f; x += 2.0f) {
		ui.mouse(x, y);
		if (GImGui->HoveredId == id) return true;
	}
	return false;
}

const EditorRequest *going(const std::vector<EditorRequest> &requests, bool back, uint32_t steps = 1) {
	const EditorRequest *request = one(requests, back ? EditorRequestKind::NavigateBack : EditorRequestKind::NavigateForward);
	return request && request->steps == steps ? request : nullptr;
}

// The arrows lead the menu bar: disabled with nowhere to go (pressed, nothing raised; the tooltip says
// what makes a place), enabled once a move kept one, the tooltip naming it and the keys; pressed, Back
// and Forward; a right-click lists Back's places nearest first, a click on the second going two back.
void test_arrows() {
	Navigating n("opennova_editor_ui_navigation_arrows");
	const SessionView &v = n.view();
	const ImGuiWindow *bar = ImGui::FindWindowByName("##MainMenuBar");
	CHECK(bar != nullptr, "the main menu bar");
	CHECK(hover(n.ui, arrow(true)), "Back's arrow on the bar");
	std::string text = logged_frame(n.ui);
	CHECK(text.find("Back (Alt+Left, or the mouse's back button): nowhere yet") != std::string::npos,
	      "nowhere to go back to: the tooltip says what makes a place");
	n.ui.activate(arrow(true));
	n.ui.activate(arrow(false));
	CHECK(n.ui.drain().empty(), "nowhere to go: the arrows raise nothing");

	n.go(request::open_document("main.mnu"));
	n.go(request::create_file("extra.mnu", asset_kind_token(AssetKind::Menu)));
	n.go(request::create_file("third.mnu", asset_kind_token(AssetKind::Menu)));
	CHECK(v.navigation.back.size() == 2 && v.navigation.forward.empty(), "two places back");
	CHECK(hover(n.ui, arrow(true)), "Back's arrow");
	text = logged_frame(n.ui);
	CHECK(text.find("Back to extra.mnu") != std::string::npos && text.find("Alt+Left") != std::string::npos,
	      "Back's tooltip names where it goes and its keys");
	n.ui.away();
	n.ui.activate(arrow(false));
	CHECK(n.ui.drain().empty(), "nowhere forward: Forward raises nothing");
	n.ui.activate(arrow(true));
	std::vector<EditorRequest> requests = n.serve();
	CHECK(going(requests, true) != nullptr && v.documents.active.find("extra.mnu") != std::string::npos, "Back");
	n.ui.activate(arrow(false));
	requests = n.serve();
	CHECK(going(requests, false) != nullptr && v.documents.active.find("third.mnu") != std::string::npos, "Forward");

	// A right-click lists Back's places, nearest first; the second goes two back.
	CHECK(hover(n.ui, arrow(true)), "Back's arrow");
	n.ui.button(true, ImGuiMouseButton_Right);
	n.ui.button(false, ImGuiMouseButton_Right);
	text = logged_frame(n.ui);
	const size_t extra = text.find("extra.mnu"), main = text.find("main.mnu");
	CHECK(extra != std::string::npos && main != std::string::npos && extra < main, "the places back, nearest first");
	const ImGuiID popup = ImHashStr("places", 0, item_id(menu_bar_id(), {"navigate_back"}));
	n.ui.activate(popup_item(popup, "###1"));
	requests = n.serve();
	CHECK(going(requests, true, 2) != nullptr && v.documents.active.find("main.mnu") != std::string::npos,
	      "the list's second place: two back");
	CHECK(v.navigation.forward.size() == 2, "the place stepped over Forward's, then where it was");
	n.ui.away();
}

// Alt+Left and Alt+Right, wherever the keyboard is but a text field with it (the field keeps its
// arrows); the mouse's back and forward buttons wherever the pointer is; none while a dialog that takes
// the whole editor shows.
void test_keys_and_buttons() {
	Navigating n("opennova_editor_ui_navigation_keys");
	const SessionView &v = n.view();
	n.go(request::open_document("main.mnu"));
	n.go(request::create_file("extra.mnu", asset_kind_token(AssetKind::Menu)));
	n.ui.focus("Document");
	n.ui.drain();
	const auto chord = [&](std::initializer_list<ImGuiKey> keys) {
		for (const ImGuiKey key : keys) n.ui.key(key, true);
		for (auto key = std::rbegin(keys); key != std::rend(keys); ++key) n.ui.key(*key, false);
		return n.serve();
	};
	std::vector<EditorRequest> requests = chord({ImGuiMod_Alt, ImGuiKey_LeftArrow});
	CHECK(going(requests, true) != nullptr && requests.size() == 1 && v.documents.active.find("main.mnu") != std::string::npos,
	      "Alt+Left: Back");
	requests = chord({ImGuiMod_Alt, ImGuiKey_RightArrow});
	CHECK(going(requests, false) != nullptr && requests.size() == 1 && v.documents.active.find("extra.mnu") != std::string::npos,
	      "Alt+Right: Forward");
	CHECK(chord({ImGuiKey_LeftArrow}).empty() && chord({ImGuiMod_Ctrl, ImGuiKey_LeftArrow}).empty(),
	      "Left alone, or with Ctrl: no Back");

	// The mouse's buttons over a window of the pass: 3 back, 4 forward.
	n.ui.mouse(900.0f, 500.0f);
	n.ui.button(true, 3);
	n.ui.button(false, 3);
	requests = n.serve();
	CHECK(going(requests, true) != nullptr && v.documents.active.find("main.mnu") != std::string::npos, "the back button");
	n.ui.button(true, 4);
	n.ui.button(false, 4);
	requests = n.serve();
	CHECK(going(requests, false) != nullptr && v.documents.active.find("extra.mnu") != std::string::npos, "the forward button");

	// A text field with the keyboard (Files' filter, activated for input): Alt+Left is its own.
	const ImGuiID files = Ui::window_id("Files");
	ImGui::ActivateItemByID(item_id(files, {"##filter"}));
	GImGui->NavNextActivateFlags = ImGuiActivateFlags_PreferInput;
	n.ui.frames(2);
	CHECK(ImGui::GetIO().WantTextInput, "the filter has the keyboard");
	requests = chord({ImGuiMod_Alt, ImGuiKey_LeftArrow});
	CHECK(only(requests, EditorRequestKind::NavigateBack) == nullptr, "a text field with the keyboard keeps Alt+Left");
	ImGui::ClearActiveID();
	n.ui.frames(1);

	// A dialog that takes the whole editor: no Back by any of them.
	n.go(request::set_workspace(R"({"new_project": {"open": true}})"));
	n.ui.frames(2);
	n.ui.activate(arrow(true));
	CHECK(only(n.ui.drain(), EditorRequestKind::NavigateBack) == nullptr, "a dialog shows: Back's arrow raises nothing");
	n.ui.button(true, 3);
	n.ui.button(false, 3);
	CHECK(only(chord({ImGuiMod_Alt, ImGuiKey_LeftArrow}), EditorRequestKind::NavigateBack) == nullptr,
	      "a dialog shows: neither the keys nor the button go back");
	n.go(request::set_workspace(R"({"new_project": {"open": false}})"));
	n.ui.frames(2);
	n.ui.activate(arrow(true));
	CHECK(going(n.ui.drain(), true) != nullptr, "the dialog closed: Back again");
	n.ui.away();
}

// A Back or a Forward to a file's page shows its About tab over the active document's, which stays the
// active one (a show_document view event); to a document, the document's tab.
void test_page_tab() {
	Navigating n("opennova_editor_ui_navigation_page");
	const SessionView &v = n.view();
	std::string font;
	for (const AssetEntry &entry : v.project.scan->entries)
		if (entry.kind == AssetKind::Font) {
			font = entry.relative_path;
			break;
		}
	CHECK(!font.empty(), "a font, a file the editor has no editor for");
	const auto page_shows = [&] {
		n.ui.frames(2);
		return logged_frame(n.ui).find("Read by the game") != std::string::npos;
	};
	n.go(request::open_document("main.mnu"));
	n.go(request::open_document(font));
	CHECK(page_shows(), "the font's page shows");
	n.go(request::create_file("extra.mnu", asset_kind_token(AssetKind::Menu)));
	CHECK(!page_shows(), "a new menu's tab over the page");
	n.go(request::navigate_back());
	CHECK(page_shows() && v.documents.active.find("extra.mnu") != std::string::npos,
	      "Back: the page's tab again, the menu still the active document");
	n.go(request::navigate_back());
	CHECK(!page_shows() && v.documents.active.find("main.mnu") != std::string::npos, "Back: the main menu's tab");
	n.go(request::navigate_forward());
	CHECK(page_shows(), "Forward: the page's tab");
	n.go(request::navigate_forward());
	CHECK(!page_shows() && v.documents.active.find("extra.mnu") != std::string::npos, "Forward: the new menu's tab");
}

} // namespace

void run_navigation_tests() {
	test_arrows();
	test_keys_and_buttons();
	test_page_tab();
}

} // namespace editor_ui_test
