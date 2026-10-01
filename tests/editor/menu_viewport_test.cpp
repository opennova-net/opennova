// The menu's viewport (editor/preview/menu_viewport, ADR 0046 S9j, S13 V5): which screen it shows (a
// menu opened, its first), the honest reason when it cannot, when its device configures again, and
// the envelope the MCP reads. The viewport compiles its screen headless itself (MenuScreenRender:
// the compiler, MenuFrameAssets through the project's files, a texture header probe), the geometry
// its hit tests and drags read; the Shell's MenuFrame does the same with Godot's decoders. A new
// project's STARTUP screen shows its title where the game draws it; a hit at its centre picks it; a
// drag of it (S9k1) lands on the grid as one undo step; an unsaved stylesheet or string table edit
// shows at once; a menu the game could not read says so and is not tried again every frame; the
// options hold a window in a state (a SetViewport). The MCP's half: its drag and its arrange
// planned as the canvas plans them (MenuViewport::drag, command), one undo step each, and a
// SetViewport's options read whole or refused whole.

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
#include <editor/preview/menu_layout_edit.h>
#include <editor/preview/menu_screen_render.h>
#include <editor/preview/menu_viewport.h>
#include <editor/preview/viewport_json.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <runtime/menu/menu_frame.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"
#include "editor/viewport_test_support.h"

using namespace opennova::editor;
using opennova::io::JsonValue;

namespace {

using editor_test::NoProcess;

const JsonValue *widget_named(const JsonValue &json, const std::string &name) {
	const JsonValue *widgets = json.get("items");
	if (!widgets) return nullptr;
	for (const JsonValue &widget : widgets->array)
		if (widget.get_string("name", "") == name) return &widget;
	return nullptr;
}

double rect_at(const JsonValue &widget, size_t i, const char *key = "rect") {
	const JsonValue *rect = widget.get(key);
	return rect && rect->array.size() == 4 ? rect->array[i].number : 0.0;
}

void set(ProjectSession &session, const Document &document, const NodeAddress &address, const char *field, Value value) {
	EditorRequest request = request::edit_record(document.path(), Edit());
	request.edits[0].address = address;
	request.edits[0].field = field;
	request.edits[0].value = std::move(value);
	session.handle(request);
}

// The menu the Preview follows, its viewport and its device: the Shell's pump, then what the device
// took, and the envelope.
struct Rig {
	ProjectSession &session;
	editor_test::FakeDevices devices;
	const SessionView &view() const { return session.view(); }
	const std::string &path() const { return view().documents.previews[ViewportKind::Menu].path; }
	ViewportAction pump() {
		devices.sync(session);
		return devices.last(path(), ViewportKind::Menu);
	}
	const MenuViewport *viewport() {
		return static_cast<const MenuViewport *>(session.viewports().find(path(), ViewportKind::Menu));
	}
	// Its envelope; none kept, the kind's over no document (editor_test::empty_viewport_json).
	JsonValue json() {
		return viewport() ? viewport_to_json(view(), *viewport(), JsonPage())
						  : editor_test::empty_viewport_json(view(), ViewportKind::Menu);
	}
	ViewportContext context(float snap = 0.0f) { return viewport_context(session.view(), *viewport(), snap); }
	size_t configures() { return viewport() ? viewport()->configures() : 0; }
};

} // namespace

static int test_headless_viewport() {
	editor_test::TempProjectDir dir("opennova_editor_menu_viewport");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	Rig rig{session};
	const SessionView &view = session.view();
	TEST_EXPECT(rig.pump() == ViewportAction::Keep && !rig.viewport());
	JsonValue json = rig.json();
	TEST_EXPECT(json.get_string("status", "") == "empty" && json.get_string("reason", "") == "no_project");
	TEST_EXPECT(json.get_string("message", "") == "Open a project to preview its menus.");
	TEST_EXPECT(json.get_string("kind", "") == "menu" && json.get_string("units", "") == "design");
	TEST_EXPECT(!json.get("device")->get_bool("attached", true));

	session.handle(request::new_project(dir.file("project"), "Preview Test"));
	session.run_operations();
	editor_test::create_missing_files(session);
	rig.pump();
	TEST_EXPECT(rig.json().get_string("reason", "") == "no_menu");
	session.handle(request::open_document("main.mnu"));
	Document *menu = session.document_for("main.mnu");
	TEST_EXPECT(menu && !menu->rows().empty());
	NodeAddress main, title, exit;
	TEST_EXPECT(find_definition(AssetGraph(), *menu, "MAIN", main) &&
			find_definition(AssetGraph(), *menu, "TITLE", title) &&
			find_definition(AssetGraph(), *menu, "EXIT", exit));
	// A menu open with none of its screens the Preview's: it asks for one.
	{
		SessionView unselected = view;
		unselected.documents.previews[ViewportKind::Menu] = PreviewTarget();
		TEST_EXPECT(editor_test::empty_viewport_json(unselected, ViewportKind::Menu).get_string("reason", "") ==
				"no_screen");
	}

	// Opened, the menu shows its first screen: STARTUP as the game draws it, the title laid
	// out in its font from fonts/.
	TEST_EXPECT(rig.path() == menu->path() && view.documents.previews[ViewportKind::Menu].part == menu->rows()[0]->id);
	TEST_EXPECT(rig.pump() == ViewportAction::Rebuild && rig.configures() == 1);
	EditorRequest select = request::select_record(menu->path(), title);
	session.handle(select);
	json = rig.json();
	TEST_EXPECT(json.get_string("status", "") == "ready" && json.get_bool("current", false));
	TEST_EXPECT(json.get("device")->get_bool("attached", false) && json.get_number("builds", 0) == 1.0);
	TEST_EXPECT(json.get("body")->get("missing") && json.get("body")->get("missing")->array.empty());
	TEST_EXPECT(json.get("body")->get("unreadable") && json.get("body")->get("unreadable")->array.empty());
	TEST_EXPECT(json.get("body")->get("screen")->get_string("name", "") == "STARTUP");
	const JsonValue *title_json = widget_named(json, "TITLE");
	TEST_EXPECT(title_json && title_json->get_string("text", "") == "Preview Test");
	TEST_EXPECT(title_json->get_number("id", 0) == double(title.child));
	TEST_EXPECT(rect_at(*title_json, 3) > rect_at(*title_json, 1)); // a text-sized height
	TEST_EXPECT(rect_at(*title_json, 1) == 75 + 120);                  // absolute: MAIN's top + its own
	TEST_EXPECT(title_json->get_string("font", "") == "Arial16b.fnt");
	TEST_EXPECT(title_json->get_string("text_color", "") == "FFFFFFFF");
	// The device placed it where the viewport's compile did (its report).
	for (size_t i = 0; i < 4; ++i) TEST_EXPECT(rect_at(*title_json, i, "device_rect") == rect_at(*title_json, i));
	// A hit at its centre picks it, as the game's pump would claim it.
	const float cx = float(rect_at(*title_json, 0) + rect_at(*title_json, 2)) / 2;
	const float cy = float(rect_at(*title_json, 1) + rect_at(*title_json, 3)) / 2;
	ViewportHit hit = rig.viewport()->hit(rig.context(), cx, cy);
	TEST_EXPECT(hit.name == "TITLE" && hit.id == title.child && hit.current && hit.kind == "static");
	TEST_EXPECT(rig.viewport()->hit(rig.context(), 5, 5).index == -1); // above MAIN: nothing
	// Nothing moved: nothing to do (the selection moved the held window, but nothing is held).
	TEST_EXPECT(rig.pump() == ViewportAction::Keep && rig.configures() == 1);

	// A drag of TITLE (S9k1): two steps of one gesture from where the viewport shows it, snapped
	// on the screen's grid under MAIN's top of 75, then the gesture's end; the viewport shows it
	// there, and one undo puts it back.
	{
		auto *mnu = static_cast<MnuDocument *>(menu);
		const MenuScreenRender &render = rig.viewport()->render();
		LayoutStart start;
		int index = -1;
		TEST_EXPECT(layout_start(*mnu, title, render.compiler(), render.state(), start, &index));
		TEST_EXPECT(index == mnu->window_index(title) && start.parent_y == 75 && start.local.top == 120);
		TEST_EXPECT(start.authored.has_left && start.authored.has_right && !start.authored.has_bottom);
		const uint64_t gesture = next_edit_gesture();
		const int before = int(rect_at(*title_json, 1));
		for (const int step : {0, 1}) {
			const LayoutDrag drag{LayoutHandle::Move, step ? 20 : 13, step ? -3 : 5, kLayoutGrid};
			std::vector<Edit> edits;
			TEST_EXPECT(layout_drag_edits(*menu, title, index, render.compiler(), start, drag, gesture, edits));
			TEST_EXPECT(!edits.empty());
			for (const Edit &edit : edits) TEST_EXPECT(edit.gesture == gesture);
			EditorRequest request = request::edit_record(menu->path(), edits);
			session.handle(request);
			TEST_EXPECT(session.last_edit_ok());
		}
		session.handle(request::end_edit(menu->path()));
		TEST_EXPECT(rig.pump() == ViewportAction::Rebuild);
		const JsonValue moved_json = rig.json();
		const JsonValue *moved = widget_named(moved_json, "TITLE");
		TEST_EXPECT(moved && rect_at(*moved, 0) == 24 && rect_at(*moved, 1) == 192);
		TEST_EXPECT(int(rect_at(*moved, 0)) % 8 == 0 && int(rect_at(*moved, 1)) % 8 == 0);
		Value bottom;
		TEST_EXPECT(!menu->present(title, "position.bottom")); // its text still sizes it
		TEST_EXPECT(menu->get(title, "position.top", bottom) && std::get<int64_t>(bottom) == 117);
		session.handle(request::undo(menu->path()));
		TEST_EXPECT(!menu->dirty());
		TEST_EXPECT(rig.pump() == ViewportAction::Rebuild);
		const JsonValue back_json = rig.json();
		const JsonValue *back = widget_named(back_json, "TITLE");
		TEST_EXPECT(back && rect_at(*back, 0) == 0 && int(rect_at(*back, 1)) == before);

		// The editor MCP's drag (MenuViewport::drag): the canvas's plan in one step from where the
		// viewport shows TITLE, snapped on the grid (0 + 13 -> 16, 195 + 5 -> 200), one undo step;
		// refused while the picture is of another revision (no pump since the edit), and for a
		// record that is not a window of the screen.
		ViewportDrag drag;
		drag.id = title.child;
		drag.handle = "move";
		drag.x = 13;
		drag.y = 5;
		drag.snap = 1;
		editor_test::Gathered planned;
		std::string error;
		TEST_EXPECT(rig.viewport()->drag(rig.context(), drag, planned, error));
		TEST_EXPECT(planned.requests.size() == 2 && planned.requests[0].kind == EditorRequestKind::EditRecord &&
				planned.requests[1].kind == EditorRequestKind::EndEdit);
		TEST_EXPECT(editor_test::serve(session, planned.requests) && menu->dirty());
		editor_test::Gathered stale;
		drag.x = 8;
		drag.y = 0;
		TEST_EXPECT(!rig.viewport()->drag(rig.context(), drag, stale, error) && stale.requests.empty() &&
				error.find("as it is now") != std::string::npos);
		TEST_EXPECT(rig.pump() == ViewportAction::Rebuild);
		const JsonValue dragged_json = rig.json();
		const JsonValue *dragged = widget_named(dragged_json, "TITLE");
		TEST_EXPECT(dragged && rect_at(*dragged, 0) == 16 && rect_at(*dragged, 1) == 200);
		session.handle(request::undo(menu->path()));
		TEST_EXPECT(!menu->dirty());
		TEST_EXPECT(rig.pump() == ViewportAction::Rebuild);
		drag.id = menu->rows()[0]->id;
		drag.x = drag.y = 8;
		TEST_EXPECT(!rig.viewport()->drag(rig.context(), drag, planned, error));
		drag.id = title.child;
		drag.handle = "middle";
		TEST_EXPECT(!rig.viewport()->drag(rig.context(), drag, planned, error) &&
				error.find("Unknown handle") != std::string::npos);
		TEST_EXPECT(!menu->dirty());
		// Its arrange (MenuViewport::command): EXIT's left edge to TITLE's, one batch; too few
		// windows, a record the screen does not show and an unknown command refused.
		editor_test::Gathered arranged;
		TEST_EXPECT(!rig.viewport()->command(rig.context(), "align_left", {title.child}, arranged, error));
		TEST_EXPECT(!rig.viewport()->command(rig.context(), "align_left", {999999, exit.child}, arranged, error));
		TEST_EXPECT(!rig.viewport()->command(rig.context(), "align_middle", {title.child, exit.child}, arranged, error));
		TEST_EXPECT(arranged.requests.empty());
		TEST_EXPECT(rig.viewport()->command(rig.context(), "align_left", {title.child, exit.child}, arranged, error));
		TEST_EXPECT(arranged.requests.size() == 1 && editor_test::serve(session, arranged.requests));
		TEST_EXPECT(rig.pump() == ViewportAction::Rebuild);
		const JsonValue aligned_json = rig.json();
		TEST_EXPECT(rect_at(*widget_named(aligned_json, "EXIT"), 0) == rect_at(*widget_named(aligned_json, "TITLE"), 0));
		session.handle(request::undo(menu->path()));
		TEST_EXPECT(!menu->dirty());
		TEST_EXPECT(rig.pump() == ViewportAction::Rebuild);

		// An operation holding the documents (S13 A3: the session takes no edit): the drag and the
		// arrange plan nothing and say why, the menu as it was.
		const uint64_t held = menu->revision();
		TEST_EXPECT(session.start_operation(std::make_unique<editor_test::HoldingOperation>()) != 0);
		editor_test::Gathered holding;
		drag.handle = "move";
		drag.x = 13;
		drag.y = 5;
		TEST_EXPECT(!rig.viewport()->drag(rig.context(), drag, holding, error) && holding.requests.empty() &&
				error.find("takes no edit") != std::string::npos);
		TEST_EXPECT(!rig.viewport()->command(rig.context(), "align_left", {title.child, exit.child}, holding, error));
		TEST_EXPECT(holding.requests.empty() && menu->revision() == held && !menu->dirty());
		session.run_operations();
	}

	// An unsaved stylesheet edit shows (the open document stands in for its file), while the
	// stylesheet is the active document: the Preview keeps the menu.
	session.handle(request::open_document("menu_style.mns"));
	Document *style = session.document_for("menu_style.mns");
	TEST_EXPECT(style && rig.path() == menu->path());
	NodeAddress fg;
	TEST_EXPECT(find_definition(AssetGraph(), *style, "DEF_TEXT_FG", fg));
	set(session, *style, fg, "value", std::string("FFFF0000"));
	TEST_EXPECT(style->dirty());
	TEST_EXPECT(rig.pump() == ViewportAction::Rebuild);
	TEST_EXPECT(widget_named(rig.json(), "TITLE")->get_string("text_color", "") == "FFFF0000");

	// An edit of a document the screen does not read changes nothing it draws.
	session.handle(request::open_document("items.def"));
	Document *items = session.document_for("items.def");
	TEST_EXPECT(items && !items->rows().empty());
	const size_t configured = rig.configures();
	set(session, *items, {items->rows()[0]->id, items->rows()[0]->kind, 0}, "type", int64_t(0));
	TEST_EXPECT(rig.pump() == ViewportAction::Keep && rig.configures() == configured);

	// The title from a string table: MAIN names menutxt.bin, TITLE's STRING is an id in
	// its Menu section; an unsaved edit of the table's text shows at once.
	session.handle(request::create_file("menutxt.bin", "strings"));
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
	EditorRequest write = request::edit_record(menu->path(), Edit());
	write.edits[0].operation = EditOperation::Write;
	write.edits[0].address = main;
	write.edits[0].field = "text_rsrc";
	session.handle(write);
	set(session, *menu, main, "text_rsrc", std::string("menutxt.bin"));
	set(session, *menu, title, "string.type", std::string("ID"));
	set(session, *menu, title, "string.value", std::string("MM_Exit"));
	TEST_EXPECT(rig.pump() == ViewportAction::Rebuild);
	TEST_EXPECT(widget_named(rig.json(), "TITLE")->get_string("text", "") == "Exit");
	set(session, *strings, exit_string, "text", std::string("Leave"));
	TEST_EXPECT(strings->dirty());
	TEST_EXPECT(rig.pump() == ViewportAction::Rebuild);
	TEST_EXPECT(widget_named(rig.json(), "TITLE")->get_string("text", "") == "Leave");

	// A table the project does not have: the ids show raw and the viewport names it.
	set(session, *menu, main, "text_rsrc", std::string("nosuch.bin"));
	rig.pump();
	json = rig.json();
	TEST_EXPECT(widget_named(json, "TITLE")->get_string("text", "") == "MM_Exit");
	TEST_EXPECT(json.get("body")->get("missing")->array.size() == 1 &&
			json.get("body")->get("missing")->array[0].string == "nosuch.bin");
	TEST_EXPECT(json.get("body")->get("unreadable")->array.empty());

	// A table the project has that does not parse: named apart from one it lacks (the
	// rescan keeps the unsaved menu open as it is).
	TEST_EXPECT(editor_test::write_text(view.project.root + "/text/broken.bin", "not a table"));
	session.handle(request::rescan());
	session.run_operations();
	TEST_EXPECT(session.document_for("main.mnu") == menu && menu->dirty());
	set(session, *menu, main, "text_rsrc", std::string("broken.bin"));
	rig.pump();
	json = rig.json();
	TEST_EXPECT(widget_named(json, "TITLE")->get_string("text", "") == "MM_Exit");
	TEST_EXPECT(json.get("body")->get("missing")->array.empty());
	TEST_EXPECT(json.get("body")->get("unreadable")->array.size() == 1 &&
			json.get("body")->get("unreadable")->array[0].string == "broken.bin");

	// A menu the game could not read: the reason says why, and it is not tried again until
	// the menu changes (the failure latch).
	set(session, *menu, exit, "string.justify", std::string("CEN\"TER"));
	TEST_EXPECT(rig.pump() == ViewportAction::Clear);
	json = rig.json();
	TEST_EXPECT(json.get_string("status", "") == "failed" && json.get_string("reason", "") == "unserializable");
	TEST_EXPECT(json.get_string("message", "").find("The game could not read this menu as it stands: ") == 0);
	TEST_EXPECT(json.get("items")->array.empty());
	const size_t failed_at = rig.configures();
	TEST_EXPECT(rig.pump() == ViewportAction::Keep && rig.configures() == failed_at);
	session.handle(request::undo(menu->path()));
	TEST_EXPECT(rig.pump() == ViewportAction::Rebuild);
	TEST_EXPECT(rig.json().get_string("status", "") == "ready");

	// Options through a SetViewport: a device size, every window shown, a window held in a state
	// (on the viewport's compile after each configure, and the device's).
	session.handle(request::set_viewport(menu->path(),
			R"({"kind": "menu", "device": {"width": 1024, "height": 768}, "options": {"show_hidden": true,)"
			R"( "force_id": )" + std::to_string(exit.child) + R"(, "force_state": "mouseover"}})"));
	TEST_EXPECT(session.outcome().done());
	TEST_EXPECT(rig.pump() == ViewportAction::Rebuild);
	TEST_EXPECT(rig.viewport()->forced_index() == static_cast<MnuDocument *>(menu)->window_index(exit));
	json = rig.json();
	TEST_EXPECT(json.get("options")->get_number("force_id", 0) == double(exit.child));
	TEST_EXPECT(json.get("options")->get_string("force_state", "") == "mouseover");
	TEST_EXPECT(json.get("device")->get_number("width", 0) == 1024 && json.get("device")->get_number("height", 0) == 768);
	// On the compile's frame state: every window shown, EXIT under the mouse.
	const auto row_of = [&](int index) -> const opennova::menu::MenuWidgetState * {
		for (const opennova::menu::MenuWidgetState &row : rig.viewport()->render().state().widgets)
			if (row.index == index) return &row;
		return nullptr;
	};
	const opennova::menu::MenuFrameCompiler &compiler = rig.viewport()->render().compiler();
	for (int index = 0; index < compiler.widget_count(); ++index)
		TEST_EXPECT(row_of(index) && row_of(index)->show && !row_of(index)->hide);
	const int forced = rig.viewport()->forced_index();
	TEST_EXPECT(row_of(forced)->hovered && !row_of(forced)->pressed && !row_of(forced)->focused);
	// Pressed, checked, its list open and focused (the caret in its shown phase): each held.
	session.handle(request::set_viewport(menu->path(),
			R"({"options": {"force_state": "selected", "checked": true, "popup_open": true, "focus": true}})"));
	TEST_EXPECT(rig.pump() == ViewportAction::Rebuild);
	TEST_EXPECT(row_of(forced)->hovered && row_of(forced)->pressed && row_of(forced)->has_checked &&
	            row_of(forced)->checked && row_of(forced)->popup_open && row_of(forced)->focused);
	TEST_EXPECT((rig.viewport()->render().state().time_ms & 0x3FFu) > 0x200u);
	const JsonValue held = rig.json();
	TEST_EXPECT(held.get("options")->get_string("force_state", "") == "selected" &&
	            held.get("options")->get_bool("checked", false) && held.get("options")->get_bool("popup_open", false) &&
	            held.get("options")->get_bool("focus", false));
	session.handle(request::set_viewport(menu->path(), R"({"options": {"force_state": "disabled"}})"));
	rig.pump();
	TEST_EXPECT(row_of(forced)->has_disabled && row_of(forced)->disabled);
	// The held state follows the selection: TITLE selected (a static: no check, no list, no caret)
	// is the window held, what its type cannot hold let go.
	session.handle(request::select_record(menu->path(), title));
	TEST_EXPECT(rig.pump() == ViewportAction::Rebuild);
	TEST_EXPECT(rig.viewport()->options().force_window == title.child && !rig.viewport()->options().checked &&
			!rig.viewport()->options().popup_open && !rig.viewport()->options().focused);
	int state = 0;
	TEST_EXPECT(menu_force_state_from_token("disabled", state) && state == opennova::menu::kStateDisabled);
	TEST_EXPECT(!menu_force_state_from_token("sideways", state));
	TEST_EXPECT(std::string(menu_force_state_token(-1)) == "normal");

	// The project closed: the menu's viewport goes with its document, and its device with it.
	session.handle(request::close_project());
	EditorRequest discard = request::resolve_unsaved(UnsavedChoice::Discard);
	session.handle(discard);
	TEST_EXPECT(!session.project_open());
	rig.pump();
	TEST_EXPECT(session.viewports().size() == 0 && rig.devices.cache.size() == 0);
	TEST_EXPECT(rig.json().get_string("reason", "") == "no_project");
	std::printf("test_headless_viewport passed\n");
	return 0;
}

// A SetViewport's options (the menu viewport's apply): each member over the options held, a
// number's fraction dropped; a member unknown, of another type or out of range refuses the whole
// change and changes nothing.
static int test_options_from_json() {
	const auto parse = [](const std::string &text) {
		JsonValue json;
		std::string error;
		opennova::io::json_parse(text, json, error);
		return json;
	};
	MenuViewport viewport("menus/a.mnu");
	PreviewClock clock;
	std::string error;
	TEST_EXPECT(viewport.apply(
	        parse(R"({"kind": "menu", "device": {"width": 1024, "height": 768.5}, "options": {"show_hidden": true,)"
	              R"( "force_id": 7, "force_state": "mouseover", "checked": true, "popup_open": false, "focus": true}})"),
	        clock, error));
	const MenuViewportOptions &options = viewport.options();
	TEST_EXPECT(viewport.state().width == 1024 && viewport.state().height == 768 && options.show_hidden &&
			options.force_window == 7);
	TEST_EXPECT(options.force_state == opennova::menu::kStateMouseover && options.checked && !options.popup_open &&
	            options.focused);
	TEST_EXPECT(viewport.apply(parse(R"({"options": {"force_state": "normal"}})"), clock, error) &&
	            viewport.options().force_state == -1 && viewport.state().width == 1024);
	const MenuViewportOptions held = viewport.options();
	for (const char *bad : {R"({"device": {"width": 0}})", R"({"device": {"height": 8193}})",
	                        R"({"options": {"force_state": "sideways"}})", R"({"options": {"bogus": 1}})",
	                        R"({"options": {"focus": 1}})", R"({"options": {"force_id": -1}})",
	                        R"({"device": {"width": "800"}})", R"([])", R"({"kind": "model"})", R"({"camera": {}})",
	                        R"({"options": {"show_hidden": false}, "device": {"width": 0}})"}) {
		error.clear();
		TEST_EXPECT(!viewport.apply(parse(bad), clock, error) && !error.empty());
		TEST_EXPECT(viewport.options() == held && viewport.state().width == 1024);
	}
	std::printf("test_options_from_json passed\n");
	return 0;
}

static int test_status_messages() {
	TEST_EXPECT(menu_screen_status_message(MenuScreenStatus::NoMenu, "") == "Open a menu to preview its screens.");
	TEST_EXPECT(menu_screen_status_message(MenuScreenStatus::NoScreen, "") == "Select one of the menu's screens.");
	TEST_EXPECT(menu_screen_status_message(MenuScreenStatus::ScreenMissing, "OPTIONS") ==
	            "Screen OPTIONS is not in the menu the game would read.");
	TEST_EXPECT(std::string(menu_screen_status_token(MenuScreenStatus::ScreenMissing)) == "screen_missing");
	std::printf("test_status_messages passed\n");
	return 0;
}

int main() {
	int failures = 0;
	failures += test_status_messages();
	failures += test_options_from_json();
	failures += test_headless_viewport();
	return failures == 0 ? 0 : 1;
}
