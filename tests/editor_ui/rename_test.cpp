// S12 D9 (ADR 0046 S12): Rename everywhere from the Inspector, over a real session's weapon
// table. A weapon's name has Rename... (and F2 in the Inspector), which asks the session for the
// plan and the dialog; the dialog opens on the name, plans again as it is typed, shows the
// definition and the item using it, and Enter raises RenameSymbol for the name typed.
#include <filesystem>
#include <string>
#include <vector>

#include <editor/graph/reference_queries.h>
#include <editor/session/request_factories.h>
#include <editor/ui/inspector_layout.h>
#include "editor_ui_test_support.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace editor_ui_test {

namespace {

void serve(ProjectSession &session, const std::vector<EditorRequest> &requests) {
	for (const EditorRequest &request : requests) session.handle(request);
}

void test_rename_everywhere_ui() {
	editor_test::TempProjectDir dir("opennova_editor_ui_rename");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(request::new_project(dir.file("project"), "Rename"));
	editor_test::create_missing_files(session);
	const SessionView &v = session.view();
	const AssetEntry *weapons = v.scan.find("weapon.def");
	const AssetEntry *items = v.scan.find("items.def");
	CHECK(weapons && items, "the catalogs");
	if (!weapons || !items) return;
	const std::string weapons_path = weapons->relative_path;
	CHECK(editor_test::write_text(v.project_root + "/" + weapons_path, "weapon \"GUN_A\"\nend\n") &&
	              editor_test::write_text(v.project_root + "/" + items->relative_path,
	                                      "begin \"Carrier\"\nid 100300\ntype vehicle\nprimary_weapon GUN_A\nend\n"),
	      "the fixtures");
	session.handle(request::rescan());
	session.handle(request::open_document(weapons_path));
	const Document *document = session.document_for(weapons_path);
	NodeAddress gun;
	CHECK(document && find_definition(AssetGraph(), *document, "GUN_A", gun), "the weapon");
	if (!document || !gun.row) return;
	EditorRequest select = request::select_record(weapons_path, gun);
	session.handle(select);
	std::string section;
	for (const InspectorSection &candidate : plan_inspector(*document, gun, gun, ""))
		for (const FieldUse &field : candidate.fields)
			if (field.schema->id == "weapon_name") section = candidate.key;
	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.focus("Inspector");
	ui.away();
	ui.drain();
	const auto asks = [&](const std::vector<EditorRequest> &requests) {
		const EditorRequest *preview = only(requests, EditorRequestKind::PreviewRename);
		return preview && preview->ask_name && preview->path == weapons_path && preview->locator == document->locator(gun) &&
		       preview->field == "weapon_name" && preview->new_name == "GUN_A";
	};
	ui.activate(item_id(Ui::window_id("Inspector"), {section.c_str(), "fields", "weapon_name", "Rename..."}));
	std::vector<EditorRequest> requests = ui.drain();
	CHECK(asks(requests), "Rename... asks for the plan and the dialog on the weapon's name");
	ui.focus("Inspector");
	ui.away();
	ui.drain();
	CHECK(asks(ui.chord({ImGuiKey_F2})), "F2 in the Inspector asks the same");
	// The session plans it and asks the dialog, which opens on the name.
	serve(session, requests);
	ui.frames(3);
	CHECK(GImGui->OpenPopupStack.Size == 1 && std::string(GImGui->OpenPopupStack[0].Window->Name) == "Rename everywhere",
	      "the dialog opens");
	ImGui::GetIO().AddInputCharactersUTF8("GUN_X");
	ui.frames(2);
	// Planned again as it is typed: the name typed (the one it opened on selected, so typing
	// replaces it).
	requests = ui.drain();
	const EditorRequest *typed = requests.empty() ? nullptr : &requests.back();
	CHECK(typed && typed->kind == EditorRequestKind::PreviewRename && !typed->ask_name, "the plan asked again");
	const std::string name = typed ? typed->new_name : std::string();
	CHECK(name.size() >= 5 && name.compare(name.size() - 5, 5, "GUN_X") == 0, "the name typed");
	serve(session, requests);
	ui.frames(2);
	const std::string text = logged_frame(ui);
	CHECK(text.find("GUN_A -> " + name) != std::string::npos && text.find("primary_weapon") != std::string::npos,
	      "the definition and the item's use, before and after");
	ui.key(ImGuiKey_Enter, true);
	ui.key(ImGuiKey_Enter, false);
	requests = ui.drain();
	const EditorRequest *rename = only(requests, EditorRequestKind::RenameSymbol);
	CHECK(rename && rename->path == weapons_path && rename->field == "weapon_name" && rename->new_name == name,
	      "Enter renames it everywhere to the name typed");
	CHECK(GImGui->OpenPopupStack.Size == 0, "and the dialog closes");
}

// A name typed over whose uses now reach another definition (D9 review): brand.mns's X_COLOR
// renamed by typing leaves the menu's %X_COLOR% on menu_style.mns's, which the game reads once
// brand.mns no longer defines it; the Inspector says the use still names the saved name.
void test_hint_on_a_fallback() {
	editor_test::TempProjectDir dir("opennova_editor_ui_rename_hint");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(request::new_project(dir.file("project"), "Hint"));
	editor_test::create_missing_files(session);
	const SessionView &v = session.view();
	const AssetEntry *sheet = v.scan.find("menu_style.mns");
	CHECK(sheet != nullptr, "the stylesheet");
	if (!sheet) return;
	const std::string folder = std::filesystem::path(sheet->relative_path).parent_path().generic_string();
	const std::string brand_path = (folder.empty() ? std::string() : folder + "/") + "brand.mns";
	CHECK(editor_test::write_text(v.project_root + "/" + sheet->relative_path,
	                              test_io::read_file_text(v.project_root + "/" + sheet->relative_path) + "X_COLOR FF000000\r\n") &&
	              editor_test::write_text(v.project_root + "/" + brand_path, "X_COLOR FF102030\r\n") &&
	              editor_test::write_text(v.project_root + "/menus/f.mnu",
	                                      "<SCREEN>\r\n<NAME>F</NAME>\r\n<WINDOW TYPE=\"STATIC\" NAME=\"W\">\r\n"
	                                      "<APPEARANCE STATE=\"DEFAULT\" TYPE=\"COLOR\">%X_COLOR%</APPEARANCE>\r\n"
	                                      "</WINDOW>\r\n</SCREEN>\r\n"),
	      "the fixtures");
	session.handle(request::rescan());
	session.handle(request::open_document(brand_path));
	const Document *brand = session.document_for(brand_path);
	NodeAddress line;
	CHECK(brand && find_definition(AssetGraph(), *brand, "X_COLOR", line), "brand.mns's line");
	if (!brand || !line.row) return;
	EditorRequest select = request::select_record(brand_path, line);
	session.handle(select);
	EditorRequest rename = request::edit_record(brand_path, Edit());
	rename.edits[0].address = line;
	rename.edits[0].field = "name";
	rename.edits[0].value = std::string("Y_COLOR");
	session.handle(rename);
	CHECK(v.graph->resolve(ReferenceKind::StyleVar, "%X_COLOR%") == ReferenceStatus::Present,
	      "the use now reaches menu_style.mns's X_COLOR");
	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.focus("Inspector");
	ui.away();
	CHECK(logged_frame(ui).find("still names 'X_COLOR'") != std::string::npos, "the hint names the use of the saved name");
}

} // namespace

void run_rename_tests() {
	test_rename_everywhere_ui();
	test_hint_on_a_fallback();
}

} // namespace editor_ui_test
