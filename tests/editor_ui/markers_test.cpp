// S11e (ADR 0046 S11): what changed since the last save, and the words and keys the windows
// share, over a null ImGui backend. An edited field is marked in the inspector (its name in
// the Changed colour, a bar at its row's left) and its tooltip says what the saved file
// holds; a right click offers Revert to saved (RevertToSaved: one batch of
// Document::revert_edits); several
// records selected, a field changed on any of them is marked, and its Revert gives every one
// its saved value in one step. A block's switch is marked, reverted and shown as a field is.
// A changed and an added record are marked in the menu view's window tree and in an outline
// (an animation table's rows, by their slots' words); a field a request asks to show
// (reveal_field) opens its section, scrolls to it and lights it a moment, again each time it
// is asked. Output says when it has nothing yet, clears through the session, copies every
// line, colours a finding's line and keeps to the newest line unless scrolled up; F2
// renames the file Files has selected, and Ctrl+F gives the keyboard to the filter of the
// window that has the focus.
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <editor/documents/mnu_document.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/session_view.h>
#include <editor/ui/editor_windows.h>
#include <editor/ui/inspector_layout.h>
#include <editor/ui/ui_kit.h>
#include <formats/rtxt/rtxt.h>
#include "editor_ui_test_support.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace editor_ui_test {

namespace {

using Change = Document::RecordChange;

// The batch a Revert to saved request (RevertToSaved: each target a record's field) comes to
// in the session: each target's Document::revert_edits, in order.
std::vector<Edit> revert_batch(const Document &document, const EditorRequest &request) {
	std::vector<Edit> batch;
	for (const Edit &target : request.edits)
		for (Edit &edit : document.revert_edits(target.address, target.field)) batch.push_back(std::move(edit));
	return batch;
}

// Whether the last frame drew anything in the colour a change is marked in, in the windows
// whose names start with `prefix` (a child window's name starts with its parent's:
// "Document/windows_...").
bool drew_mark(const char *prefix, Change change) {
	const ImU32 color = ImGui::GetColorU32(ui_kit::change_color(change));
	for (ImGuiWindow *window : GImGui->Windows) {
		if (!window->Active || std::strncmp(window->Name, prefix, std::strlen(prefix)) != 0) continue;
		for (const ImDrawVert &vertex : window->DrawList->VtxBuffer)
			if (vertex.col == color) return true;
	}
	return false;
}

// A field's row of the inspector's form: the point on its name in the section `key`'s table,
// `row` rows down, found by moving along the name's line until its tooltip says `words`.
bool field_name_at(Ui &ui, const char *key, int row, const char *words, ImVec2 &at) {
	const ImGuiTable *table = ImGui::TableFindByID(item_id(Ui::window_id("Inspector"), {key, "fields"}));
	if (!table) return false;
	const float line = ImGui::GetFrameHeight() + ImGui::GetStyle().CellPadding.y * 2.0f;
	const float y = table->OuterRect.Min.y + (float(row) + 0.5f) * line;
	const ImGuiTableColumn &name = table->Columns[0];
	for (float x = name.WorkMinX + 1.0f; x < name.MaxX; x += 3.0f) {
		ui.mouse(x, y);
		if (logged_frame(ui).find(words) != std::string::npos) {
			at = ImVec2(x, y);
			return true;
		}
	}
	return false;
}

// The row a reveal lights in the inspector's last frame: its background in the header's
// active colour, fading (none of the inspector's other colours of that hue is as bright,
// short of a hover or a tick's mark: Header 79, Button 102, a hover or a check mark 204 and
// up), its top and bottom. A table draws a row's background only while the row shows,
// clipped to what shows of it, so a whole row lit is a row in sight.
bool lit_row(float &top, float &bottom) {
	const ImGuiWindow *inspector = ImGui::FindWindowByName("Inspector");
	if (!inspector) return false;
	const ImU32 hue = ImGui::GetColorU32(ImGuiCol_HeaderActive) & ~IM_COL32_A_MASK;
	top = FLT_MAX;
	bottom = -FLT_MAX;
	for (const ImDrawVert &vertex : inspector->DrawList->VtxBuffer) {
		const ImU32 alpha = (vertex.col & IM_COL32_A_MASK) >> IM_COL32_A_SHIFT;
		if ((vertex.col & ~IM_COL32_A_MASK) != hue || alpha < 110 || alpha > 200) continue;
		top = std::min(top, vertex.pos.y);
		bottom = std::max(bottom, vertex.pos.y);
	}
	return top < bottom;
}

// Whether the last frame lit a whole row of the inspector's form inside what it shows.
bool lit_in_sight() {
	const ImGuiWindow *inspector = ImGui::FindWindowByName("Inspector");
	float top = 0.0f, bottom = 0.0f;
	return inspector && lit_row(top, bottom) && bottom - top >= ImGui::GetFrameHeight() &&
	       top >= inspector->InnerClipRect.Min.y - 1.0f && bottom <= inspector->InnerClipRect.Max.y + 1.0f;
}

Edit set_edit(const NodeAddress &address, const char *field, Value value) {
	Edit edit;
	edit.address = address;
	edit.field = field;
	edit.value = std::move(value);
	return edit;
}

// BACK's left edge moved: its Left marked, the tooltip naming the saved 10; its right edge,
// left out of the saved file, written: "Left out of the saved file". A right click on Left
// offers Revert to saved, a RevertToSaved request the session makes one batch of the
// document's own revert edits. BACK is
// marked Changed in the window tree, a window added marked Added; saved again (a document
// read fresh), nothing is marked. BACK and TITLE selected together: a field changed on TITLE
// alone is marked in the shared form.
void test_field_marks() {
	editor_test::TempProjectDir dir("opennova_editor_ui_marks_test");
	std::shared_ptr<MnuDocument> document = load_menu(dir);
	const NodeAddress back = named(*document, "BACK"), title = named(*document, "TITLE"), main = named(*document, "MAIN");
	SessionView v = menu_view(document);
	select_in(v, back);
	Ui ui;
	ImGui::GetIO().DisplaySize = ImVec2(1280.0f, 720.0f);
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.focus("Inspector");
	ui.away();
	ui.drain();
	CHECK(!drew_mark("Inspector", Change::Changed) && !drew_mark("Document/windows", Change::Changed) &&
	              !drew_mark("Document/windows", Change::Added),
	      "saved: nothing marked");

	Diagnostic error;
	CHECK(document->apply(set_edit(back, "position.left", int64_t(20)), error), "BACK's left moved");
	CHECK(document->apply(set_edit(back, "position.right", int64_t(300)), error), "BACK's right written");
	v.revisions.touch(ViewConcern::Documents);
	ui.frames(3);
	ui.away();
	ui.frames();
	CHECK(drew_mark("Inspector", Change::Changed), "the changed fields marked");
	ImVec2 left;
	CHECK(field_name_at(ui, "position", 0, "Saved: 10", left), "Left's tooltip: the saved value");
	ImVec2 right;
	CHECK(field_name_at(ui, "position", 2, "Left out of the saved file", right), "Right's tooltip: left out of the saved file");

	// A right click on Left: Revert to saved, the RevertToSaved request on BACK's Left, which
	// the session makes the document's own revert edits, one batch.
	ui.mouse(left.x, left.y);
	ui.button(true, 1);
	ui.button(false, 1);
	ui.frames();
	const ImGuiID revert = item_id(Ui::window_id("Inspector"), {"position", "fields", "position.left", "revert"});
	ui.activate(popup_item(revert, "Revert to saved"));
	std::vector<EditorRequest> requests = ui.drain();
	const EditorRequest *reverting = one(requests, EditorRequestKind::RevertToSaved);
	const std::vector<Edit> batch = reverting ? revert_batch(*document, *reverting) : std::vector<Edit>();
	CHECK(reverting && reverting->path == document->path() && reverting->edits.size() == 1 &&
	              reverting->edits[0].address == back && reverting->edits[0].field == "position.left" &&
	              batch.size() == 1 && batch[0].operation == EditOperation::Set && batch[0].address == back &&
	              std::get<int64_t>(batch[0].value) == 10,
	      "Revert to saved: one batch, the saved value back");
	ImGui::ClosePopupsExceptModals();
	ui.away();

	// The window tree: BACK changed; a window added under MAIN.
	CHECK(drew_mark("Document/windows", Change::Changed) && !drew_mark("Document/windows", Change::Added),
	      "BACK marked in the tree");
	Edit add;
	add.operation = EditOperation::Add;
	add.address = {main.row, node_kind(MenuKind::Window), 0};
	add.parent = main.child;
	CHECK(document->apply(add, error), "a window added");
	v.revisions.touch(ViewConcern::Documents);
	ui.frames(3);
	CHECK(drew_mark("Document/windows", Change::Added), "the added window marked");

	// BACK and TITLE together, TITLE's Hidden set: the shared form marks it.
	std::shared_ptr<MnuDocument> fresh = load_menu(dir);
	replace_view(v, menu_view(fresh));
	v.selection = named(*fresh, "BACK");
	v.selected = {named(*fresh, "BACK"), named(*fresh, "TITLE")};
	v.revisions.touch(ViewConcern::Selection);
	ui.frames(3);
	CHECK(!drew_mark("Inspector", Change::Changed) && !drew_mark("Document/windows", Change::Changed), "read fresh: nothing");
	CHECK(fresh->apply(set_edit(named(*fresh, "TITLE"), "hidden", int64_t(1)), error), "TITLE hidden");
	v.revisions.touch(ViewConcern::Documents);
	ui.frames(3);
	CHECK(drew_mark("Inspector", Change::Changed), "a field changed on one of the selected records is marked");
	(void)title;
	ui.drain();
}

// An animation table's rows in its outline, by their slots' words: a row whose key changed
// marked Changed, a row added marked Added, both unmarked once undone. Its row selected, the
// inspector's breadcrumb names it by the same words, the key in the tooltip.
void test_outline_marks() {
	editor_test::TempProjectDir dir("opennova_editor_ui_outline_marks");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	CHECK(preview_project(session, dir), "the preview project");
	session.handle(make_request(EditorRequestKind::OpenDocument, "anims/SKIN.adm"));
	Document *table = session.document_for("anims/SKIN.adm");
	CHECK(table && !table->rows().empty(), "the animation table");
	if (!table || table->rows().empty()) return;
	const SessionView &v = session.view();
	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.away();
	CHECK(!drew_mark("Document/outline", Change::Changed) && !drew_mark("Document/outline", Change::Added), "saved: none");
	std::string text = logged_frame(ui);
	CHECK(text.find("walk forward") != std::string::npos && text.find("anim_walk_forward") == std::string::npos,
	      "a row by its slot's words, not its key");
	NodeAddress walk;
	CHECK(table->find("anim_walk_forward", walk), "the walk row");
	EditorRequest pick = make_request(EditorRequestKind::SelectRecord, table->path());
	pick.edit.address = walk;
	session.handle(pick);
	ui.frames(3);
	ui.away();
	text = logged_frame(ui);
	CHECK(text.find("anim_walk_forward") == std::string::npos && count_of(text, "walk forward") >= 2,
	      "selected: the outline and the breadcrumb both by its words");
	const Node &row = *table->rows().back();
	EditorRequest key = make_request(EditorRequestKind::EditRecord, table->path());
	key.edit = set_edit({row.id, row.kind, 0}, "key", std::string("anim_idle_2"));
	session.handle(key);
	ui.frames(3);
	CHECK(drew_mark("Document/outline", Change::Changed) && !drew_mark("Document/outline", Change::Added), "the row changed");
	EditorRequest add = make_request(EditorRequestKind::EditRecord, table->path());
	add.edit.operation = EditOperation::Add;
	add.edit.address = {0, row.kind, 0};
	session.handle(add);
	ui.frames(3);
	CHECK(drew_mark("Document/outline", Change::Added), "the row added");
	session.handle(make_request(EditorRequestKind::Undo, table->path()));
	session.handle(make_request(EditorRequestKind::Undo, table->path()));
	ui.frames(3);
	CHECK(!table->dirty() && !drew_mark("Document/outline", Change::Changed) && !drew_mark("Document/outline", Change::Added),
	      "undone: none");
}

// A field the view asks to show (a Problems row's): its section, folded while the file leaves
// it out, opens, the form scrolls the field itself into sight and lights its row, the light
// gone after a moment. Scrolled away, the same ask again (the same row clicked again: the
// view's serial moved) shows and lights it again; a view that moved otherwise does not.
void test_reveal_field() {
	editor_test::TempProjectDir dir("opennova_editor_ui_reveal_test");
	std::shared_ptr<MnuDocument> document = load_menu(dir);
	const NodeAddress back = named(*document, "BACK");
	SessionView v = menu_view(document);
	select_in(v, back);
	Ui ui;
	ImGui::GetIO().DisplaySize = ImVec2(1280.0f, 600.0f);
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.away();
	// The last section of BACK's form that holds fields and starts folded (nothing of it written).
	const std::vector<InspectorSection> plan = plan_inspector(*document, back, back, "");
	const InspectorSection *folded = nullptr;
	for (const InspectorSection &section : plan)
		if (!section.key.empty() && !section.written && !section.fields.empty()) folded = &section;
	CHECK(folded != nullptr, "a folded section with fields");
	if (!folded) return;
	const ImGuiID table_id = item_id(Ui::window_id("Inspector"), {folded->key.c_str(), "fields"});
	const ImGuiWindow *inspector = ImGui::FindWindowByName("Inspector");
	CHECK(inspector && inspector->Scroll.y == 0.0f, "the form at its top");
	const ImGuiTable *before = ImGui::TableFindByID(table_id);
	CHECK(!before || before->LastFrameActive != GImGui->FrameCount, "its fields not drawn while folded");
	v.reveal_field = folded->fields.back().schema->id;
	++v.reveal_serial;
	v.revisions.touch(ViewConcern::Selection);
	ui.frames(4);
	const ImGuiTable *table = ImGui::TableFindByID(table_id);
	CHECK(table && table->LastFrameActive == GImGui->FrameCount, "its section opened");
	CHECK(inspector && inspector->Scroll.y > 0.0f, "the form scrolled");
	CHECK(table && inspector && table->OuterRect.Max.y > inspector->Pos.y &&
	              table->OuterRect.Min.y < inspector->Pos.y + inspector->Size.y,
	      "its section in sight");
	float top = 0.0f, bottom = 0.0f;
	CHECK(lit_in_sight() && lit_row(top, bottom) && table && bottom > table->OuterRect.Max.y - (bottom - top) * 1.5f,
	      "the field itself (its section's last row) in sight and lit");
	ui.frames(80);
	CHECK(!lit_row(top, bottom), "the light gone after its moment");

	// Back at the top: a view that moved for something else shows nothing again; the same
	// field asked again does, and lights it again.
	ImGui::SetScrollY(ImGui::FindWindowByName("Inspector"), 0.0f);
	ui.frames(2);
	v.revisions.touch(ViewConcern::Output);
	ui.frames(3);
	CHECK(inspector && inspector->Scroll.y == 0.0f && !lit_row(top, bottom), "the same ask is shown once");
	++v.reveal_serial;
	v.revisions.touch(ViewConcern::Selection);
	ui.frames(4);
	CHECK(inspector && inspector->Scroll.y > 0.0f && lit_in_sight(), "asked again: scrolled to and lit again");
	ui.drain();
}

// Whether the last frame drew a selected item's highlight (the header colour) in the windows
// whose names start with `prefix` (a child window's name starts with its parent's): the
// selection is in view there. A logged frame draws every item, so the reveal is read here.
bool drew_selected(const char *prefix) {
	const ImU32 color = ImGui::GetColorU32(ImGuiCol_Header);
	for (ImGuiWindow *window : GImGui->Windows) {
		if (!window->Active || std::strncmp(window->Name, prefix, std::strlen(prefix)) != 0) continue;
		for (const ImDrawVert &vertex : window->DrawList->VtxBuffer)
			if (vertex.col == color) return true;
	}
	return false;
}

// How far the window whose name starts with `prefix` (and none of its children) is scrolled
// down; -1 when there is none.
float scrolled(const char *prefix) {
	for (ImGuiWindow *window : GImGui->Windows)
		if (window->Active && std::strncmp(window->Name, prefix, std::strlen(prefix)) == 0 &&
		    !std::strchr(window->Name + std::strlen(prefix), '/'))
			return window->Scroll.y;
	return -1.0f;
}

// A Go to reveals its record in the Document tab's view (S12 Z2): into a long string table the
// strings list scrolls to the key (and back up to the first one; a key in view moves nothing),
// and into a model the outline opens the model and its user points to show the point.
void test_reveal_in_views() {
	editor_test::TempProjectDir dir("opennova_editor_ui_reveal_views");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	CHECK(preview_project(session, dir), "the preview project");
	const SessionView &v = session.view();
	opennova::rtxt::File table;
	table.sections = {{"Menu", 200}};
	for (int i = 0; i < 200; ++i) {
		char key[16], text[16];
		std::snprintf(key, sizeof(key), "KEY_%03d", i);
		std::snprintf(text, sizeof(text), "Text %03d", i);
		opennova::rtxt::Entry entry;
		entry.key = key;
		entry.text = text;
		table.entries.push_back(entry);
	}
	std::vector<uint8_t> bytes;
	std::string io_error;
	CHECK(opennova::rtxt::write(table, bytes, io_error) && editor_test::write_bytes(v.project_root + "/strings/many.bin", bytes),
	      "a table of 200 keys");
	session.handle(make_request(EditorRequestKind::Rescan));
	const AssetEntry *many = v.scan.find("many.bin");
	CHECK(many != nullptr, "the table scanned");
	if (!many) return;
	const std::string strings_path = many->relative_path;
	session.handle(make_request(EditorRequestKind::OpenDocument, strings_path));
	const Document *strings = session.document_for(strings_path);
	NodeAddress first, near, middle, late;
	CHECK(strings && strings->find("KEY_000", first) && strings->find("KEY_003", near) && strings->find("KEY_100", middle) &&
	              strings->find("KEY_180", late),
	      "the keys");
	if (!strings || !first.child || !near.child || !middle.child || !late.child) return;
	const auto go_to = [&](const std::string &path, const std::string &locator, const char *field) {
		EditorRequest open = make_request(EditorRequestKind::OpenDocument, path, locator);
		open.edit.field = field;
		session.handle(open);
	};
	const char *const list = "Document/strings_";
	go_to(strings_path, strings->locator(first), "key");
	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.away();
	CHECK(drew_selected(list) && scrolled(list) == 0.0f, "the first key: the list at its top");
	// From another file, a Go to a late key: its tab shown, the list scrolled to it.
	session.handle(make_request(EditorRequestKind::OpenDocument, "main.mnu"));
	ui.frames(3);
	go_to(strings_path, strings->locator(late), "key");
	ui.frames(4);
	CHECK(drew_selected(list) && scrolled(list) > 0.0f, "scrolled to the late key");
	go_to(strings_path, strings->locator(middle), "key");
	ui.frames(4);
	CHECK(drew_selected(list) && scrolled(list) > 0.0f, "to a middle one");
	go_to(strings_path, strings->locator(first), "key");
	ui.frames(4);
	CHECK(drew_selected(list) && scrolled(list) == 0.0f, "back up to the first");
	go_to(strings_path, strings->locator(near), "key");
	ui.frames(4);
	CHECK(drew_selected(list) && scrolled(list) == 0.0f, "a key in view: nothing moves");

	// A model's user point: the outline, collapsed, opens to it.
	session.handle(make_request(EditorRequestKind::OpenDocument, "models/armory.3di"));
	const Document *model = session.document_for("models/armory.3di");
	CHECK(model && !model->rows().empty(), "the model");
	if (!model || model->rows().empty()) return;
	const NodeAddress row{model->rows()[0]->id, model->rows()[0]->kind, 0};
	NodeAddress point;
	for (const Document::Collection &collection : model->collections_of(row))
		if (std::string(collection.spec.kind_name) == "user_point" && !collection.ids.empty())
			point = {row.row, collection.spec.kind, collection.ids.back()};
	CHECK(point.child != 0, "a user point");
	if (!point.child) return;
	EditorRequest pick = make_request(EditorRequestKind::SelectRecord, model->path());
	pick.edit.address = row;
	session.handle(pick);
	ui.frames(4);
	CHECK(drew_selected("Document/outline"), "the model row selected, its node closed");
	session.handle(make_request(EditorRequestKind::OpenDocument, "main.mnu"));
	ui.frames(3);
	go_to(model->path(), model->locator(point), "name");
	ui.frames(4);
	CHECK(v.selection == point && drew_selected("Document/outline"), "the outline opened to the point");
	ui.drain();
}

// TITLE's Text block (its STRING) left out: its section stays open (a field of it changed),
// and asked to show the switch (the first row of the block's form), the form scrolls to it
// from the form's end and lights it; the switch is marked as a field is, its tooltip naming
// the saved Yes; Revert to saved raises the request whose batch writes the block again, one step
// undone as one.
void test_block_switch() {
	editor_test::TempProjectDir dir("opennova_editor_ui_switch_test");
	std::shared_ptr<MnuDocument> document = load_menu(dir);
	const NodeAddress title = named(*document, "TITLE");
	SessionView v = menu_view(document);
	select_in(v, title);
	Ui ui;
	ImGui::GetIO().DisplaySize = ImVec2(1280.0f, 420.0f);
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.away();
	CHECK(!drew_mark("Inspector", Change::Changed), "saved: nothing marked");
	Diagnostic error;
	CHECK(document->apply(set_edit(title, "string", int64_t(0)), error), "the block left out");
	v.revisions.touch(ViewConcern::Documents);
	ui.frames(3);

	// From the form's end, the switch asked to show: its row (the first of its block's form)
	// in sight and lit.
	ImGuiWindow *inspector = ImGui::FindWindowByName("Inspector");
	CHECK(inspector && inspector->ScrollMax.y > 0.0f, "the form scrolls");
	if (!inspector) return;
	ImGui::SetScrollY(inspector, inspector->ScrollMax.y);
	ui.frames(2);
	v.reveal_field = "string";
	++v.reveal_serial;
	v.revisions.touch(ViewConcern::Selection);
	ui.frames(4);
	const ImGuiTable *form = ImGui::TableFindByID(item_id(Ui::window_id("Inspector"), {"string", "fields"}));
	float top = 0.0f, bottom = 0.0f;
	CHECK(lit_in_sight() && lit_row(top, bottom) && form && std::fabs(top - form->OuterRect.Min.y) <= 1.0f,
	      "the switch shown and lit");

	// Marked as a field is.
	ui.away();
	ui.frames();
	CHECK(drew_mark("Inspector", Change::Changed), "the switch marked");
	ImVec2 at;
	CHECK(field_name_at(ui, "string", 0, "Saved: Yes", at), "its tooltip: the saved Yes");
	CHECK(logged_frame(ui).find("In the file") != std::string::npos, "the switch's row");

	// Revert to saved: the block written again in one batch; applied, then undone as one step.
	ui.mouse(at.x, at.y);
	ui.button(true, 1);
	ui.button(false, 1);
	ui.frames();
	const ImGuiID revert = item_id(Ui::window_id("Inspector"), {"string", "fields", "string", "revert"});
	ui.activate(popup_item(revert, "Revert to saved"));
	std::vector<EditorRequest> requests = ui.drain();
	const EditorRequest *reverting = one(requests, EditorRequestKind::RevertToSaved);
	const std::vector<Edit> batch = reverting ? revert_batch(*document, *reverting) : std::vector<Edit>();
	CHECK(batch.size() == 1 && batch[0].address == title && batch[0].field == "string" &&
	              std::get<int64_t>(batch[0].value) == 1,
	      "Revert to saved: the block written again");
	ImGui::ClosePopupsExceptModals();
	ui.away();
	if (!batch.empty()) {
		Value value;
		CHECK(document->apply(batch, error) && document->get(title, "string", value) &&
		              std::get<int64_t>(value) == 1 && !document->field_changed(title, "string"),
		      "reverted: written, unmarked");
		document->undo();
		CHECK(document->get(title, "string", value) && std::get<int64_t>(value) == 0 && document->field_changed(title, "string"),
		      "one undo: left out again");
	}
	v.revisions.touch(ViewConcern::Documents);
	ui.frames(3);
	ui.drain();
}

// BACK and TITLE selected together, Hidden set on both in one step: the shared form marks it,
// its tooltip naming each record's saved value, and its Revert to saved is one batch giving
// both their saved value, undone as one step.
void test_revert_several() {
	editor_test::TempProjectDir dir("opennova_editor_ui_revert_several");
	std::shared_ptr<MnuDocument> document = load_menu(dir);
	const NodeAddress back = named(*document, "BACK"), title = named(*document, "TITLE");
	SessionView v = menu_view(document);
	v.selection = back;
	v.selected = {back, title};
	v.revisions.touch(ViewConcern::Selection);
	Ui ui;
	ImGui::GetIO().DisplaySize = ImVec2(1280.0f, 720.0f);
	ui.windows.set_view(&v);
	ui.frames(6);
	Diagnostic error;
	CHECK(document->apply(std::vector<Edit>{set_edit(back, "hidden", int64_t(1)), set_edit(title, "hidden", int64_t(1))}, error),
	      "both hidden in one step");
	v.revisions.touch(ViewConcern::Documents);
	ui.frames(3);
	ui.away();
	ui.frames();
	// Hidden's row in the shared form's general fields.
	int row = -1;
	const std::vector<InspectorSection> shared = plan_shared_inspector(*document, {back, title}, "");
	for (size_t i = 0; !shared.empty() && shared.front().key.empty() && i < shared.front().fields.size(); ++i)
		if (shared.front().fields[i].schema->id == "hidden") row = int(i);
	CHECK(row >= 0, "Hidden is a shared field");
	ImVec2 at;
	CHECK(field_name_at(ui, "", row, "BACK: Saved: No", at) && logged_frame(ui).find("TITLE: Saved: No") != std::string::npos,
	      "its tooltip: each record's saved value");
	ui.mouse(at.x, at.y);
	ui.button(true, 1);
	ui.button(false, 1);
	ui.frames();
	const ImGuiID revert = item_id(Ui::window_id("Inspector"), {"", "fields", "hidden", "revert"});
	ui.activate(popup_item(revert, "Revert to saved"));
	std::vector<EditorRequest> requests = ui.drain();
	const EditorRequest *reverting = one(requests, EditorRequestKind::RevertToSaved);
	const std::vector<Edit> batch = reverting ? revert_batch(*document, *reverting) : std::vector<Edit>();
	CHECK(reverting && reverting->edits.size() == 2 && batch.size() == 2 && batch[0].address == back &&
	              batch[1].address == title && std::get<int64_t>(batch[0].value) == 0 &&
	              std::get<int64_t>(batch[1].value) == 0,
	      "Revert to saved: one batch over both");
	ImGui::ClosePopupsExceptModals();
	if (batch.empty()) return;
	Value a, b;
	CHECK(document->apply(batch, error) && document->get(back, "hidden", a) && document->get(title, "hidden", b) &&
	              std::get<int64_t>(a) == 0 && std::get<int64_t>(b) == 0 && !document->field_changed(back, "hidden") &&
	              !document->field_changed(title, "hidden"),
	      "both back to their saved value");
	document->undo();
	CHECK(document->get(back, "hidden", a) && document->get(title, "hidden", b) && std::get<int64_t>(a) == 1 &&
	              std::get<int64_t>(b) == 1,
	      "one undo: both hidden again");
	ui.drain();
}

// Where a copy lands: the clipboard the test stands in for the platform's.
std::string g_clipboard;
void keep_clipboard(ImGuiContext *, const char *text) { g_clipboard = text ? text : ""; }

// Output: "Nothing yet." while empty; Clear raises the session's ClearOutput, Copy puts every
// line on the clipboard; a finding's line in its severity's colour.
void test_output_window() {
	SessionView v;
	v.project_open = true;
	v.project_root = "C:/mods/Output";
	v.document.title = "Output";
	Ui ui;
	ImGui::GetPlatformIO().Platform_SetClipboardTextFn = keep_clipboard;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.focus("Output");
	ui.away();
	CHECK(logged_frame(ui).find("Nothing yet.") != std::string::npos, "empty: nothing yet");
	for (const char *line : {"Opened Output.", "error: A broken thing.", "warning: A doubtful thing.", "game: a line"})
		v.output.append(line);
	v.revisions.touch(ViewConcern::Output);
	ui.frames(3);
	CHECK(logged_frame(ui).find("Nothing yet.") == std::string::npos, "lines: no empty state");
	const ImU32 error = ImGui::GetColorU32(ui_kit::severity_color(DiagnosticSeverity::Error));
	const ImU32 warning = ImGui::GetColorU32(ui_kit::severity_color(DiagnosticSeverity::Warning));
	bool red = false, amber = false;
	for (ImGuiWindow *window : GImGui->Windows) {
		if (!window->Active || std::strncmp(window->Name, "Output/lines", 12) != 0) continue;
		for (const ImDrawVert &vertex : window->DrawList->VtxBuffer) {
			red = red || vertex.col == error;
			amber = amber || vertex.col == warning;
		}
	}
	CHECK(red && amber, "a finding's line in its severity's colour");
	const ImGuiID output = Ui::window_id("Output");
	ui.activate(item_id(output, {"Copy"}));
	CHECK(g_clipboard == "Opened Output.\nerror: A broken thing.\nwarning: A doubtful thing.\ngame: a line\n",
	      "Copy: every line");
	ui.activate(item_id(output, {"Clear"}));
	std::vector<EditorRequest> requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::ClearOutput) != nullptr, "Clear: the session's ClearOutput");
	ImGui::GetPlatformIO().Platform_SetClipboardTextFn = nullptr;
}

// The child window Output lists its lines in.
ImGuiWindow *output_lines() {
	for (ImGuiWindow *window : GImGui->Windows)
		if (window->Active && std::strncmp(window->Name, "Output/lines", 12) == 0) return window;
	return nullptr;
}

// Output keeps to the newest line while it shows the bottom: lines that arrive scroll it on.
// Scrolled up to read, it stays where it was as more arrive.
void test_output_follows() {
	SessionView v;
	v.project_open = true;
	v.project_root = "C:/mods/Output";
	v.document.title = "Output";
	for (int i = 0; i < 200; ++i) v.output.append("line " + std::to_string(i));
	Ui ui;
	ImGui::GetIO().DisplaySize = ImVec2(1280.0f, 720.0f);
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.focus("Output");
	ui.away();
	ImGuiWindow *lines = output_lines();
	CHECK(lines && lines->ScrollMax.y > 0.0f && lines->Scroll.y == lines->ScrollMax.y, "opened at the newest line");
	if (!lines) return;
	const float before = lines->ScrollMax.y;
	for (int i = 0; i < 20; ++i) v.output.append("more " + std::to_string(i));
	v.revisions.touch(ViewConcern::Output);
	ui.frames(3);
	CHECK(lines->ScrollMax.y > before && lines->Scroll.y == lines->ScrollMax.y, "new lines followed");
	// Scrolled up to read: more lines leave it there.
	ImGui::SetScrollY(lines, 0.0f);
	ui.frames(2);
	CHECK(lines->Scroll.y == 0.0f, "scrolled up");
	for (int i = 0; i < 20; ++i) v.output.append("later " + std::to_string(i));
	v.revisions.touch(ViewConcern::Output);
	ui.frames(3);
	CHECK(lines->Scroll.y == 0.0f && lines->ScrollMax.y > before, "scrolled up: kept where it was");
	// At the log's cap the count of lines stands while they move on: the newest is followed still.
	ImGui::SetScrollY(lines, lines->ScrollMax.y);
	for (size_t i = v.output.size(); i < OutputLog::kMaxLines; ++i) v.output.append("filler " + std::to_string(i));
	v.revisions.touch(ViewConcern::Output);
	ui.frames(3);
	CHECK(v.output.size() == OutputLog::kMaxLines && lines->Scroll.y == lines->ScrollMax.y, "at the cap: at the newest line");
	ImGui::SetScrollY(lines, lines->ScrollMax.y - 1.0f);
	ui.frames(1);
	for (int i = 0; i < 20; ++i) v.output.append("past the cap " + std::to_string(i));
	v.revisions.touch(ViewConcern::Output);
	ui.frames(3);
	CHECK(v.output.size() == OutputLog::kMaxLines && lines->Scroll.y == lines->ScrollMax.y, "past the cap: followed");
	CHECK(ui.drain().empty(), "raising nothing");
}

// F2 in Files renames the file it has selected (the name asked in the same popup as its
// menu's Rename...); Ctrl+F gives the keyboard to the filter of the window with the focus.
void test_shortcuts() {
	SessionView v;
	v.project_open = true;
	v.project_root = "C:/mods/Keys";
	v.document.title = "Keys";
	v.scan.entries = {file_entry("items.def", "defs/items.def", AssetKind::ItemDefs),
	                  file_entry("readme.txt", "readme.txt", AssetKind::Text)};
	v.scan.index();
	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.focus("Files");
	ui.away();
	ui.drain();
	const ImGuiID files = Ui::window_id("Files");
	// Ctrl+F: the Files filter takes the keyboard.
	ui.chord({ImGuiMod_Ctrl, ImGuiKey_F});
	ui.frames(2);
	CHECK(GImGui->ActiveId == item_id(files, {"##filter"}) && ImGui::GetIO().WantTextInput, "Ctrl+F: the filter of Files");
	ImGui::ClearActiveID();
	ui.frames(2);
	// A file selected with a click, then F2: the rename popup, with its name.
	const ImGuiWindow *window = ImGui::FindWindowByName("Files");
	const ImGuiID row = item_id(files, {"files", "readme.txt", "##row"});
	bool selected = false;
	for (float y = window->Pos.y; y < window->Pos.y + window->Size.y && !selected; y += 2.0f) {
		ui.mouse(window->Pos.x + window->Size.x * 0.3f, y);
		if (GImGui->HoveredId != row) continue;
		ui.button(true);
		ui.button(false);
		selected = true;
	}
	CHECK(selected, "readme.txt clicked");
	ui.away();
	ui.chord({ImGuiKey_F2});
	ui.frames(2);
	const std::string text = logged_frame(ui);
	CHECK(text.find("Rename readme.txt to") != std::string::npos, "F2: the rename of the selected file");
	ImGui::ClosePopupsExceptModals();
	ui.frames(2);
	// The Inspector's filter from its own Ctrl+F: none shows with no record, so nothing takes it.
	ui.focus("Inspector");
	ui.chord({ImGuiMod_Ctrl, ImGuiKey_F});
	ui.frames(2);
	CHECK(!ImGui::GetIO().WantTextInput, "no filter shown: nothing takes the keyboard");
	CHECK(ui.drain().empty(), "raising nothing");
}

} // namespace

void run_marker_tests() {
	test_field_marks();
	test_outline_marks();
	test_reveal_field();
	test_reveal_in_views();
	test_block_switch();
	test_revert_several();
	test_output_window();
	test_output_follows();
	test_shortcuts();
}

} // namespace editor_ui_test
