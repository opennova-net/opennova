// The menu preview's portable half (editor/preview): which screen it shows (a menu opened,
// its first), the honest status when it cannot, when the device must configure again, and
// the JSON the MCP reads. The device here is the engine's own (the compiler,
// MenuFrameAssets through the project's files, a texture header probe): the shell's
// MenuFrame does the same with Godot's decoders. A new project's STARTUP screen shows its
// title where the game draws it; a click at its centre picks it; a drag of it (S9k1) lands
// on the grid as one undo step; an unsaved stylesheet or string table edit shows at once; a
// menu the game could not read says so and is not retried every frame; the options hold a
// window in a state. The editor MCP's half: its options read from JSON, its drag and its
// arrange planned as the pane plans them, one undo step each.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <base/io/json.h>
#include <editor/assets/project_asset_source.h>
#include <editor/documents/mnu_document.h>
#include <editor/graph/reference_queries.h>
#include <editor/preview/menu_arrange.h>
#include <editor/preview/menu_layout_edit.h>
#include <editor/preview/menu_preview_json.h>
#include <editor/preview/menu_preview_state.h>
#include <editor/preview/texture_header.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/view/session_view.h>
#include <runtime/menu/menu_frame.h>
#include <runtime/menu/menu_frame_assets.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
using opennova::io::JsonValue;

namespace {

using editor_test::NoProcess;

// A headless device: what MenuFrame::configure_screen does, without the pixels.
struct Device {
	MenuPreviewModel model;
	opennova::menu::MenuFrameCompiler compiler;
	opennova::menu::MenuFrameState state;
	opennova::menu::MenuFrameAssets assets;
	TextureHeaderProbe decoder;
	int configures = 0;

	MenuPreviewAction pump(const SessionView &view) {
		const MenuPreviewAction action = model.follow(view);
		if (action == MenuPreviewAction::Configure) {
			++configures;
			state = opennova::menu::MenuFrameState();
			assets.configure(compiler, model.image(), model.screen(), *view.findings.assets, decoder, model.style_vars());
			model.configured(assets);
			apply_menu_preview_options(model.options(), model.forced_index(), compiler, state);
		} else if (action == MenuPreviewAction::Clear) {
			assets.clear(compiler, decoder);
		}
		return action;
	}
	JsonValue json(const SessionView &view) const {
		return menu_preview_to_json(menu_preview_snapshot(view, model, &compiler, &state));
	}
	JsonValue hit(const SessionView &view, float x, float y) const {
		return menu_preview_hit_to_json(menu_preview_snapshot(view, model, &compiler, &state), x, y);
	}
};

const JsonValue *widget_named(const JsonValue &json, const std::string &name) {
	const JsonValue *widgets = json.get("widgets");
	if (!widgets) return nullptr;
	for (const JsonValue &widget : widgets->array)
		if (widget.get_string("name", "") == name) return &widget;
	return nullptr;
}

double rect_at(const JsonValue &widget, size_t i) {
	const JsonValue *rect = widget.get("rect");
	return rect && rect->array.size() == 4 ? rect->array[i].number : 0.0;
}

void set(ProjectSession &session, const Document &document, const NodeAddress &address, const char *field, Value value) {
	EditorRequest request = make_request(EditorRequestKind::EditRecord, document.path());
	request.edit.address = address;
	request.edit.field = field;
	request.edit.value = std::move(value);
	session.handle(request);
}

} // namespace

static int test_headless_preview() {
	editor_test::TempProjectDir dir("opennova_editor_menu_preview");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	Device device;
	const SessionView &view = session.view();
	TEST_EXPECT(device.pump(view) == MenuPreviewAction::Keep);
	TEST_EXPECT(device.json(view).get_string("status", "") == "no_project");
	TEST_EXPECT(device.json(view).get_string("message", "") == "Open a project to preview its menus.");
	TEST_EXPECT(menu_preview_to_json(menu_preview_snapshot(view, device.model, nullptr, nullptr)).get_string("status", "") ==
	            "no_device");

	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Preview Test"));
	editor_test::create_missing_files(session);
	device.pump(view);
	TEST_EXPECT(device.json(view).get_string("status", "") == "no_menu");
	session.handle(make_request(EditorRequestKind::OpenDocument, "main.mnu"));
	Document *menu = session.document_for("main.mnu");
	TEST_EXPECT(menu && !menu->rows().empty());
	NodeAddress main, title, exit;
	TEST_EXPECT(find_definition(AssetGraph(), *menu, "MAIN", main) &&
			find_definition(AssetGraph(), *menu, "TITLE", title) &&
			find_definition(AssetGraph(), *menu, "EXIT", exit));
	// A menu open with none of its screens the preview's: it asks for one.
	{
		SessionView unselected = view;
		unselected.documents.previews.menu = DocumentsView::MenuPreviewTarget();
		Device other;
		other.pump(unselected);
		TEST_EXPECT(other.json(unselected).get_string("status", "") == "no_screen");
	}

	// Opened, the menu shows its first screen: STARTUP as the game draws it, the title laid
	// out in its font from fonts/.
	TEST_EXPECT(view.documents.previews.menu.path == menu->path() && view.documents.previews.menu.screen == menu->rows()[0]->id);
	TEST_EXPECT(device.pump(view) == MenuPreviewAction::Configure);
	EditorRequest select = make_request(EditorRequestKind::SelectRecord, menu->path());
	select.edit.address = title;
	session.handle(select);
	JsonValue json = device.json(view);
	TEST_EXPECT(json.get_string("status", "") == "ready" && json.get_bool("current", false));
	TEST_EXPECT(json.get("missing") && json.get("missing")->array.empty());
	TEST_EXPECT(json.get("unreadable") && json.get("unreadable")->array.empty());
	const JsonValue *title_json = widget_named(json, "TITLE");
	TEST_EXPECT(title_json && title_json->get_string("text", "") == "Preview Test");
	TEST_EXPECT(title_json->get_number("id", 0) == double(title.child));
	TEST_EXPECT(rect_at(*title_json, 3) > rect_at(*title_json, 1)); // a text-sized height
	TEST_EXPECT(rect_at(*title_json, 1) == 75 + 120);                  // absolute: MAIN's top + its own
	TEST_EXPECT(title_json->get_string("font", "") == "Arial16b.fnt");
	TEST_EXPECT(title_json->get_string("text_color", "") == "FFFFFFFF");
	// A click at its centre picks it, as the game's pump would claim it.
	const float cx = float(rect_at(*title_json, 0) + rect_at(*title_json, 2)) / 2;
	const float cy = float(rect_at(*title_json, 1) + rect_at(*title_json, 3)) / 2;
	JsonValue hit = device.hit(view, cx, cy);
	TEST_EXPECT(hit.get_string("name", "") == "TITLE" && hit.get_number("id", 0) == double(title.child));
	TEST_EXPECT(device.hit(view, 5, 5).get_number("index", 0) == -1); // above MAIN: nothing
	// Nothing moved: nothing to do.
	TEST_EXPECT(device.pump(view) == MenuPreviewAction::Keep && device.configures == 1);

	// A drag of TITLE (S9k1): two steps of one gesture from where the preview shows it,
	// snapped on the screen's grid under MAIN's top of 75, then the gesture's end; the
	// preview shows it there, and one undo puts it back.
	{
		auto *mnu = static_cast<MnuDocument *>(menu);
		LayoutStart start;
		int index = -1;
		TEST_EXPECT(layout_start(*mnu, title, device.compiler, device.state, start, &index));
		TEST_EXPECT(index == mnu->window_index(title) && start.parent_y == 75 && start.local.top == 120);
		TEST_EXPECT(start.authored.has_left && start.authored.has_right && !start.authored.has_bottom);
		const uint64_t gesture = next_edit_gesture();
		const int before = int(rect_at(*title_json, 1));
		for (const int step : {0, 1}) {
			const LayoutDrag drag{LayoutHandle::Move, step ? 20 : 13, step ? -3 : 5, kLayoutGrid};
			std::vector<Edit> edits;
			TEST_EXPECT(layout_drag_edits(*menu, title, index, device.compiler, start, drag, gesture, edits));
			TEST_EXPECT(!edits.empty());
			for (const Edit &edit : edits) TEST_EXPECT(edit.gesture == gesture);
			EditorRequest request = make_request(EditorRequestKind::EditRecord, menu->path());
			request.edits = edits;
			session.handle(request);
			TEST_EXPECT(session.last_edit_ok());
		}
		session.handle(make_request(EditorRequestKind::EndEdit, menu->path()));
		TEST_EXPECT(device.pump(view) == MenuPreviewAction::Configure);
		const JsonValue moved_json = device.json(view);
		const JsonValue *moved = widget_named(moved_json, "TITLE");
		TEST_EXPECT(moved && rect_at(*moved, 0) == 24 && rect_at(*moved, 1) == 192);
		TEST_EXPECT(int(rect_at(*moved, 0)) % 8 == 0 && int(rect_at(*moved, 1)) % 8 == 0);
		Value bottom;
		TEST_EXPECT(!menu->present(title, "position.bottom")); // its text still sizes it
		TEST_EXPECT(menu->get(title, "position.top", bottom) && std::get<int64_t>(bottom) == 117);
		session.handle(make_request(EditorRequestKind::Undo, menu->path()));
		TEST_EXPECT(!menu->dirty());
		TEST_EXPECT(device.pump(view) == MenuPreviewAction::Configure);
		const JsonValue back_json = device.json(view);
		const JsonValue *back = widget_named(back_json, "TITLE");
		TEST_EXPECT(back && rect_at(*back, 0) == 0 && int(rect_at(*back, 1)) == before);

		// The editor MCP's drag (menu_preview_drag): the pane's plan in one step from where
		// the preview shows TITLE, snapped on the grid (0 + 13 -> 16, 195 + 5 -> 200), one undo
		// step; refused while the picture is of another revision, and for a record that is
		// not a window of the screen.
		TEST_EXPECT(menu_preview_drag(session, menu_preview_snapshot(view, device.model, &device.compiler, &device.state),
		                              title.child, LayoutHandle::Move, 13, 5, true));
		TEST_EXPECT(menu->dirty());
		TEST_EXPECT(!menu_preview_drag(session, menu_preview_snapshot(view, device.model, &device.compiler, &device.state),
		                               title.child, LayoutHandle::Move, 8, 0, true));
		TEST_EXPECT(device.pump(view) == MenuPreviewAction::Configure);
		const JsonValue dragged_json = device.json(view);
		const JsonValue *dragged = widget_named(dragged_json, "TITLE");
		TEST_EXPECT(dragged && rect_at(*dragged, 0) == 16 && rect_at(*dragged, 1) == 200);
		session.handle(make_request(EditorRequestKind::Undo, menu->path()));
		TEST_EXPECT(!menu->dirty());
		TEST_EXPECT(device.pump(view) == MenuPreviewAction::Configure);
		const MenuPreviewSnapshot shown = menu_preview_snapshot(view, device.model, &device.compiler, &device.state);
		TEST_EXPECT(!menu_preview_drag(session, shown, menu->rows()[0]->id, LayoutHandle::Move, 8, 8, true));
		TEST_EXPECT(!menu->dirty());
		// Its arrange: EXIT's left edge to TITLE's, one batch; too few windows refused.
		TEST_EXPECT(!menu_preview_arrange(session, shown, {title.child}, ArrangeOp::AlignLeft));
		TEST_EXPECT(!menu_preview_arrange(session, shown, {999999, exit.child}, ArrangeOp::AlignLeft));
		TEST_EXPECT(menu_preview_arrange(session, shown, {title.child, exit.child}, ArrangeOp::AlignLeft));
		TEST_EXPECT(device.pump(view) == MenuPreviewAction::Configure);
		const JsonValue aligned_json = device.json(view);
		TEST_EXPECT(rect_at(*widget_named(aligned_json, "EXIT"), 0) == rect_at(*widget_named(aligned_json, "TITLE"), 0));
		session.handle(make_request(EditorRequestKind::Undo, menu->path()));
		TEST_EXPECT(!menu->dirty());
		TEST_EXPECT(device.pump(view) == MenuPreviewAction::Configure);

		// Refused by the session, an operation holding the documents (S13 A2: the drag and the
		// arrange answer what the request came to, never the last edit's flag): each answers
		// false, the menu as it was.
		const uint64_t held = menu->revision();
		TEST_EXPECT(session.start_operation(std::make_unique<editor_test::HoldingOperation>()) != 0);
		const MenuPreviewSnapshot holding = menu_preview_snapshot(view, device.model, &device.compiler, &device.state);
		TEST_EXPECT(!menu_preview_drag(session, holding, title.child, LayoutHandle::Move, 13, 5, true));
		TEST_EXPECT(!menu_preview_arrange(session, holding, {title.child, exit.child}, ArrangeOp::AlignLeft));
		TEST_EXPECT(menu->revision() == held && !menu->dirty());
		session.run_operations();
	}

	// An unsaved stylesheet edit shows (the open document stands in for its file), while the
	// stylesheet is the active document.
	session.handle(make_request(EditorRequestKind::OpenDocument, "menu_style.mns"));
	Document *style = session.document_for("menu_style.mns");
	TEST_EXPECT(style);
	NodeAddress fg;
	TEST_EXPECT(find_definition(AssetGraph(), *style, "DEF_TEXT_FG", fg));
	set(session, *style, fg, "value", std::string("FFFF0000"));
	TEST_EXPECT(style->dirty());
	TEST_EXPECT(device.pump(view) == MenuPreviewAction::Configure);
	TEST_EXPECT(widget_named(device.json(view), "TITLE")->get_string("text_color", "") == "FFFF0000");

	// An edit of a document the screen does not read changes nothing it draws.
	session.handle(make_request(EditorRequestKind::OpenDocument, "items.def"));
	Document *items = session.document_for("items.def");
	TEST_EXPECT(items && !items->rows().empty());
	set(session, *items, {items->rows()[0]->id, items->rows()[0]->kind, 0}, "type", int64_t(0));
	TEST_EXPECT(device.pump(view) == MenuPreviewAction::Keep);

	// The title from a string table: MAIN names menutxt.bin, TITLE's STRING is an id in
	// its Menu section; an unsaved edit of the table's text shows at once.
	session.handle(make_request(EditorRequestKind::CreateFile, "menutxt.bin", "strings"));
	Document *strings = session.document_for("menutxt.bin");
	TEST_EXPECT(strings);
	NodeAddress exit_string;
	for (const auto &row : strings->rows())
		for (const Document::Collection &collection : strings->collections_of({row->id, row->kind, 0}))
			for (const NodeId id : collection.ids) {
				Value key;
				const NodeAddress address{row->id, collection.spec.kind, id};
				if (strings->get(address, "key", key) && std::get<std::string>(key) == "MM_Exit") exit_string = address;
			}
	TEST_EXPECT(exit_string.child != 0);
	EditorRequest write = make_request(EditorRequestKind::EditRecord, menu->path());
	write.edit.operation = EditOperation::Write;
	write.edit.address = main;
	write.edit.field = "text_rsrc";
	session.handle(write);
	set(session, *menu, main, "text_rsrc", std::string("menutxt.bin"));
	set(session, *menu, title, "string.type", std::string("ID"));
	set(session, *menu, title, "string.value", std::string("MM_Exit"));
	TEST_EXPECT(device.pump(view) == MenuPreviewAction::Configure);
	TEST_EXPECT(widget_named(device.json(view), "TITLE")->get_string("text", "") == "Exit");
	set(session, *strings, exit_string, "text", std::string("Leave"));
	TEST_EXPECT(strings->dirty());
	TEST_EXPECT(device.pump(view) == MenuPreviewAction::Configure);
	TEST_EXPECT(widget_named(device.json(view), "TITLE")->get_string("text", "") == "Leave");

	// A table the project does not have: the ids show raw and the preview names it.
	set(session, *menu, main, "text_rsrc", std::string("nosuch.bin"));
	device.pump(view);
	json = device.json(view);
	TEST_EXPECT(widget_named(json, "TITLE")->get_string("text", "") == "MM_Exit");
	TEST_EXPECT(json.get("missing")->array.size() == 1 && json.get("missing")->array[0].string == "nosuch.bin");
	TEST_EXPECT(json.get("unreadable")->array.empty());

	// A table the project has that does not parse: named apart from one it lacks (the
	// rescan keeps the unsaved menu open as it is).
	TEST_EXPECT(editor_test::write_text(view.project.root + "/text/broken.bin", "not a table"));
	session.handle(make_request(EditorRequestKind::Rescan));
	TEST_EXPECT(session.document_for("main.mnu") == menu && menu->dirty());
	set(session, *menu, main, "text_rsrc", std::string("broken.bin"));
	device.pump(view);
	json = device.json(view);
	TEST_EXPECT(widget_named(json, "TITLE")->get_string("text", "") == "MM_Exit");
	TEST_EXPECT(json.get("missing")->array.empty());
	TEST_EXPECT(json.get("unreadable")->array.size() == 1 && json.get("unreadable")->array[0].string == "broken.bin");

	// A menu the game could not read: the status says why, and it is not retried until
	// the menu changes.
	set(session, *menu, exit, "string.justify", std::string("CEN\"TER"));
	TEST_EXPECT(device.pump(view) == MenuPreviewAction::Clear);
	json = device.json(view);
	TEST_EXPECT(json.get_string("status", "") == "unserializable");
	TEST_EXPECT(json.get_string("message", "").find("The game could not read this menu as it stands: ") == 0);
	TEST_EXPECT(json.get("widgets")->array.empty());
	TEST_EXPECT(device.pump(view) == MenuPreviewAction::Keep);
	session.handle(make_request(EditorRequestKind::Undo, menu->path()));
	TEST_EXPECT(device.pump(view) == MenuPreviewAction::Configure);
	TEST_EXPECT(device.json(view).get_string("status", "") == "ready");

	// Options: a device size, every window shown, a window held in a state (applied by the
	// device after its configure).
	MenuPreviewOptions options;
	options.show_hidden = true;
	options.force_window = exit.child;
	options.force_state = opennova::menu::kStateMouseover;
	device.model.set_options(options);
	TEST_EXPECT(device.pump(view) == MenuPreviewAction::Configure);
	TEST_EXPECT(device.model.forced_index() == static_cast<MnuDocument *>(menu)->window_index(exit));
	TEST_EXPECT(device.json(view).get("options")->get_number("force_id", 0) == double(exit.child));
	TEST_EXPECT(device.json(view).get("options")->get_string("force_state", "") == "mouseover");
	// Applied after the configure: every window shown, EXIT under the mouse.
	auto row_of = [&](int index) -> const opennova::menu::MenuWidgetState * {
		for (const opennova::menu::MenuWidgetState &row : device.state.widgets)
			if (row.index == index) return &row;
		return nullptr;
	};
	for (int index = 0; index < device.compiler.widget_count(); ++index)
		TEST_EXPECT(row_of(index) && row_of(index)->show && !row_of(index)->hide);
	const int forced = device.model.forced_index();
	TEST_EXPECT(row_of(forced)->hovered && !row_of(forced)->pressed && !row_of(forced)->focused);
	// Pressed, checked, its list open and focused (the caret in its shown phase): each held.
	options.force_state = opennova::menu::kStateSelected;
	options.checked = options.popup_open = options.focused = true;
	device.model.set_options(options);
	TEST_EXPECT(device.pump(view) == MenuPreviewAction::Configure);
	TEST_EXPECT(row_of(forced)->hovered && row_of(forced)->pressed && row_of(forced)->has_checked &&
	            row_of(forced)->checked && row_of(forced)->popup_open && row_of(forced)->focused);
	TEST_EXPECT((device.state.time_ms & 0x3FFu) > 0x200u);
	const JsonValue held = device.json(view);
	TEST_EXPECT(held.get("options")->get_string("force_state", "") == "selected" &&
	            held.get("options")->get_bool("checked", false) && held.get("options")->get_bool("popup_open", false) &&
	            held.get("options")->get_bool("focus", false));
	options.force_state = opennova::menu::kStateDisabled;
	device.model.set_options(options);
	device.pump(view);
	TEST_EXPECT(row_of(forced)->has_disabled && row_of(forced)->disabled);
	int state = 0;
	TEST_EXPECT(menu_preview_state_from_token("disabled", state) && state == opennova::menu::kStateDisabled);
	TEST_EXPECT(!menu_preview_state_from_token("sideways", state));
	TEST_EXPECT(std::string(menu_preview_state_token(-1)) == "normal");

	// The project closed: nothing to show, and the device drops what it configured.
	session.handle(make_request(EditorRequestKind::CloseProject));
	EditorRequest discard = make_request(EditorRequestKind::ResolveUnsaved);
	discard.unsaved_choice = UnsavedChoice::Discard;
	session.handle(discard);
	TEST_EXPECT(!session.project_open());
	TEST_EXPECT(device.pump(view) == MenuPreviewAction::Clear && device.compiler.widget_count() == 0);
	TEST_EXPECT(device.json(view).get_string("status", "") == "no_project");
	std::printf("test_headless_preview passed\n");
	return 0;
}

// The options the editor MCP sets (menu_preview_options_from_json): each member over the
// options held, a number's fraction dropped; a member unknown, of another type or out of
// range refuses the whole object and changes nothing.
static int test_options_from_json() {
	const auto parse = [](const char *text) {
		JsonValue json;
		std::string error;
		opennova::io::json_parse(text, json, error);
		return json;
	};
	MenuPreviewOptions options;
	TEST_EXPECT(menu_preview_options_from_json(
	        parse(R"({"width": 1024, "height": 768.5, "show_hidden": true, "force_id": 7, "force_state": "mouseover",)"
	              R"( "checked": true, "popup_open": false, "focus": true})"),
	        options));
	TEST_EXPECT(options.width == 1024 && options.height == 768 && options.show_hidden && options.force_window == 7);
	TEST_EXPECT(options.force_state == opennova::menu::kStateMouseover && options.checked && !options.popup_open &&
	            options.focused);
	TEST_EXPECT(menu_preview_options_from_json(parse(R"({"force_state": "normal"})"), options) &&
	            options.force_state == -1 && options.width == 1024);
	const MenuPreviewOptions held = options;
	for (const char *bad : {R"({"width": 0})", R"({"height": 8193})", R"({"force_state": "sideways"})", R"({"bogus": 1})",
	                        R"({"focus": 1})", R"({"force_id": -1})", R"({"width": "800"})", R"([])",
	                        R"({"show_hidden": false, "width": 0})"}) {
		TEST_EXPECT(!menu_preview_options_from_json(parse(bad), options));
		TEST_EXPECT(options == held);
	}
	std::printf("test_options_from_json passed\n");
	return 0;
}

static int test_preview_status_messages() {
	TEST_EXPECT(menu_preview_status_message(MenuPreviewStatus::NoMenu, "") == "Open a menu to preview its screens.");
	TEST_EXPECT(menu_preview_status_message(MenuPreviewStatus::NoScreen, "") == "Select one of the menu's screens.");
	TEST_EXPECT(menu_preview_status_message(MenuPreviewStatus::NoDevice, "") == "No preview renderer is attached.");
	TEST_EXPECT(menu_preview_status_message(MenuPreviewStatus::ScreenMissing, "OPTIONS") ==
	            "Screen OPTIONS is not in the menu the game would read.");
	TEST_EXPECT(std::string(menu_preview_status_token(MenuPreviewStatus::ScreenMissing)) == "screen_missing");
	std::printf("test_preview_status_messages passed\n");
	return 0;
}

int main() {
	int failures = 0;
	failures += test_preview_status_messages();
	failures += test_options_from_json();
	failures += test_headless_preview();
	return failures == 0 ? 0 : 1;
}
