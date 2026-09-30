// S12 D8 (ADR 0046 S12): find, over a real session's item and weapon tables. Ctrl+F opens the
// Document window's find bar with the keyboard in it: what is typed finds the fields whose value,
// as the Inspector shows it, holds it (a turn rate in its written degrees a second), Enter shows
// the next hit (its record selected, its field revealed: the OpenDocument a Go to raises), and
// Escape closes the bar. Ctrl+Shift+F opens Find in project: a weapon's name finds its symbol,
// whose Go to leads to the record defining it and whose opened list leads to the item using it;
// going anywhere closes the modal.
#include <editor/graph/reference_queries.h>
#include <cstdio>
#include <string>
#include <vector>

#include <editor/session/request_factories.h>

#include "editor_ui_test_support.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace editor_ui_test {

namespace {

struct FindProject {
	editor_test::TempProjectDir dir{"opennova_editor_ui_find"};
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	std::string items_path, weapons_path;
	NodeAddress item, weapon;

	bool open() {
		session.handle(request::new_project(dir.file("project"), "Find"));
		editor_test::create_missing_files(session);
		const SessionView &v = session.view();
		const AssetEntry *items = v.project.scan->find("items.def");
		const AssetEntry *weapons = v.project.scan->find("weapon.def");
		if (!items || !weapons) return false;
		items_path = items->relative_path;
		weapons_path = weapons->relative_path;
		if (!editor_test::write_text(v.project.root + "/" + items_path,
		                             "begin \"Found Thing\"\nid 100300\ntype vehicle\nturn_rate 90\nprimary_weapon Searchgun\n"
		                             "end\nbegin \"Second Thing\"\nid 100301\ntype vehicle\nturn_rate 90\nend\n"
		                             "begin \"Third Thing\"\nid 100302\ntype vehicle\nturn_rate 90\nend\n") ||
		    !editor_test::write_text(v.project.root + "/" + weapons_path, "weapon \"Searchgun\"\nend\n"))
			return false;
		session.handle(request::rescan());
		session.handle(request::open_document(weapons_path));
		const Document *weapons_document = session.document_for(weapons_path);
		session.handle(request::open_document(items_path));
		const Document *items_document = session.document_for(items_path);
		return items_document && weapons_document &&
				find_definition(AssetGraph(), *items_document, "100300", item) &&
				find_definition(AssetGraph(), *weapons_document, "Searchgun", weapon);
	}
	std::string locator(const std::string &path, const NodeAddress &address) {
		const Document *document = session.document_for(path);
		return document ? document->locator(address) : std::string();
	}
};

// The Go to raised: the file opened at the record's locator, the field shown.
bool goes_to(const std::vector<EditorRequest> &requests, const std::string &path, const std::string &locator,
             const char *field) {
	const EditorRequest *open = only(requests, EditorRequestKind::OpenDocument);
	return open && open->path == path && open->locator == locator && open->field == field;
}

void press(Ui &ui, ImGuiKey key) {
	ui.key(key, true);
	ui.key(key, false);
}

// Find in project's list of results (a child of the modal), whose id its items' ids start from.
ImGuiID results_id() {
	char name[96];
	std::snprintf(name, sizeof(name), "Find in project/results_%08X", item_id(ImHashStr("Find in project"), {"results"}));
	return ImHashStr(name);
}

bool modal_open() {
	return GImGui->OpenPopupStack.Size == 1 && GImGui->OpenPopupStack[0].Window &&
	       std::string(GImGui->OpenPopupStack[0].Window->Name) == "Find in project";
}

void test_find_bar() {
	FindProject project;
	CHECK(project.open(), "the find project");
	if (project.items_path.empty()) return;
	Ui ui;
	ui.windows.set_view(&project.session.view());
	ui.frames(6);
	ui.focus("Document");
	ui.away();
	ui.drain();
	ui.chord({ImGuiMod_Ctrl, ImGuiKey_F});
	ImGui::GetIO().AddInputCharactersUTF8("90");
	ui.frames(2);
	CHECK(ui.drain().empty(), "typing finds, and goes nowhere yet");
	CHECK(logged_frame(ui).find("3 matches") != std::string::npos, "the turn rates, in their written degrees a second");
	press(ui, ImGuiKey_Enter);
	CHECK(goes_to(ui.drain(), project.items_path, project.locator(project.items_path, project.item), "turn_rate"),
	      "Enter shows the hit: the item selected, its turn rate revealed");
	CHECK(logged_frame(ui).find("1 of 3") != std::string::npos, "the hit shown of how many");
	press(ui, ImGuiKey_Enter);
	CHECK(logged_frame(ui).find("2 of 3") != std::string::npos, "the next");
	ui.drain();
	// The hit shown stops matching: the bar is on none, and the next is the one after it.
	Document *items = project.session.document_for(project.items_path);
	NodeAddress second, third;
	CHECK(items && find_definition(AssetGraph(), *items, "100301", second) &&
					find_definition(AssetGraph(), *items, "100302", third),
			"the other items");
	if (!items) return;
	EditorRequest edit = request::edit_record(project.items_path, Edit());
	edit.edits[0].address = second;
	edit.edits[0].field = "turn_rate";
	edit.edits[0].value = int64_t(45);
	project.session.handle(edit);
	ui.frames(2);
	CHECK(logged_frame(ui).find("2 matches") != std::string::npos, "two left, none of them shown");
	press(ui, ImGuiKey_Enter);
	CHECK(goes_to(ui.drain(), project.items_path, project.locator(project.items_path, third), "turn_rate"),
	      "the next is the hit after the one that stopped matching");
	CHECK(logged_frame(ui).find("2 of 2") != std::string::npos, "shown of how many");
	press(ui, ImGuiKey_Escape);
	CHECK(logged_frame(ui).find("2 of 2") == std::string::npos, "Escape closes the bar");
	// Edit > Find... opens it again, as it was.
	CHECK(choose(ui, "Edit", {"Find..."}).empty(), "the menu opens the bar, raising nothing");
	CHECK(logged_frame(ui).find("2 of 2") != std::string::npos, "the bar is back, its text and its hit kept");
}

void test_find_in_project() {
	FindProject project;
	CHECK(project.open(), "the find project");
	if (project.items_path.empty()) return;
	Ui ui;
	ui.windows.set_view(&project.session.view());
	ui.frames(6);
	ui.away();
	ui.drain();
	ui.chord({ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiKey_F});
	CHECK(modal_open(), "Ctrl+Shift+F opens Find in project");
	ImGui::GetIO().AddInputCharactersUTF8("searchg");
	ui.frames(2);
	CHECK(logged_frame(ui).find("Searchgun") != std::string::npos, "the weapon's name found");
	const ImGuiID result = item_id(pushed(results_id(), 0), {"result"});
	ui.activate(item_id(pushed(results_id(), 0), {"Go to"}));
	CHECK(goes_to(ui.drain(), project.weapons_path, project.locator(project.weapons_path, project.weapon), "weapon_name"),
	      "Go to leads to the record defining it");
	CHECK(!modal_open(), "going closes the modal");
	// Opened again, the text kept: the result opened lists its use, a click away.
	ui.chord({ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiKey_F});
	CHECK(modal_open(), "open again");
	ui.activate(result);
	ui.activate(ImHashStr("###use", 0, pushed(result, 0)));
	CHECK(goes_to(ui.drain(), project.items_path, project.locator(project.items_path, project.item), "primary_weapon"),
	      "the use leads to the item naming it");
	CHECK(!modal_open(), "going closes the modal");
	// Escape while typing closes it too, keeping the text.
	ui.chord({ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiKey_F});
	ImGui::GetIO().AddInputCharactersUTF8("zzqq");
	ui.frames(2);
	press(ui, ImGuiKey_Escape);
	CHECK(!modal_open() && ui.drain().empty(), "Escape closes the modal");
	ui.chord({ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiKey_F});
	const std::string text = logged_frame(ui);
	CHECK(modal_open() && text.find("No file or name holds it.") != std::string::npos,
	      "reopened, it finds what was typed (zzqq: nothing)");
}

} // namespace

void run_find_tests() {
	test_find_bar();
	test_find_in_project();
}

} // namespace editor_ui_test
