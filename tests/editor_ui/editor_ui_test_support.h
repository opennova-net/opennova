// Shared scaffolding for the editor_ui ctest (editor_windows_test.cpp, workspace_test.cpp,
// markers_test.cpp, bounds_test.cpp, field_widgets_test.cpp, reference_picker_test.cpp,
// find_test.cpp, rename_test.cpp, gate_test.cpp) and the view contract (view_contract_test.cpp):
// the failure counter and CHECK, a null ImGui backend
// (a display size and a built font atlas, no platform or renderer), the workspace driven
// frame by frame with the mouse and the keys (Ui), the ids ImGui gives items, what a frame
// writes as text, what runs past the width that shows of it, the requests a test looks for, the
// menu fixture the windows are driven over, the viewports' devices a test stands in for the Shell's
// (and the viewports a hand-made view shares, pumped as the Shell pumps them), and a project with
// every preview's files.
#pragma once

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <editor/documents/mnu_document.h>
#include <editor/graph/reference_queries.h>
#include <base/io/json.h>
#include <editor/assets/project_asset_source.h>
#include <editor/import/import_plan.h>
#include <editor/preview/viewport_device.h>
#include <editor/preview/viewport_device_cache.h>
#include <editor/preview/viewport_model.h>
#include <editor/preview/viewports.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/editor_windows.h>
#include "../editor/anim_test_support.h"
#include "../editor/editor_test_support.h"
#include "../editor/test_platform.h"
#include "common/file_io.h"
#include "common/test_paths.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace editor_ui_test {

using namespace opennova::editor;
namespace devtools = opennova::devtools;

inline int g_failures = 0;

#define CHECK(cond, message)                                                          \
	do {                                                                              \
		if (!(cond)) {                                                                \
			std::printf("FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, message, #cond); \
			++editor_ui_test::g_failures;                                             \
		}                                                                             \
	} while (0)

// A headless ImGui frame: the null example's setup, no platform or renderer backend.
struct NullBackend {
	ImGuiContext *context = nullptr;
	NullBackend() {
		context = ImGui::CreateContext();
		ImGuiIO &io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.DisplaySize = ImVec2(1280.0f, 720.0f);
		io.DeltaTime = 1.0f / 60.0f;
		unsigned char *pixels = nullptr;
		int width = 0;
		int height = 0;
		io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
	}
	~NullBackend() { ImGui::DestroyContext(context); }
};

inline void *test_alloc(size_t size, void *) { return std::malloc(size); }
inline void test_free(void *ptr, void *) { std::free(ptr); }

inline bool frame(EditorWindows &windows, uint64_t index) {
	ImGui::NewFrame();
	const bool drew = windows.draw_frame(index);
	ImGui::Render();
	return drew;
}

inline const devtools::Window *find_window(const devtools::ImGuiPass &pass, const char *title) {
	for (int i = 0; i < pass.window_count(); ++i) {
		if (std::strcmp(pass.window(i).title(), title) == 0) return &pass.window(i);
	}
	return nullptr;
}

// The workspace over a 1920x1080 display unless a test sizes it, driven frame by frame; `pump`,
// when a test sets it, runs before each frame as the Shell's pump does (its viewports' devices
// following the view).
struct Ui {
	NullBackend backend;
	EditorWindows windows;
	uint64_t index = 0;
	std::function<void()> pump;
	Ui() {
		ImGui::GetIO().DisplaySize = ImVec2(1920.0f, 1080.0f);
		windows.pass().attach_imgui(backend.context, test_alloc, test_free, nullptr);
	}
	~Ui() { windows.pass().detach_imgui(); }
	void frames(int count = 1) {
		for (int i = 0; i < count; ++i) {
			if (pump) pump();
			frame(windows, ++index);
		}
	}
	void focus(const char *title) {
		for (int i = 0; i < windows.pass().window_count(); ++i)
			if (std::strcmp(windows.pass().window(i).title(), title) == 0) windows.pass().window(i).request_focus();
		frames(3);
	}
	std::vector<EditorRequest> drain() {
		std::vector<EditorRequest> out;
		EditorRequest request;
		while (windows.take_request(request)) out.push_back(request);
		return out;
	}
	void mouse(float x, float y) {
		ImGui::GetIO().AddMousePosEvent(x, y);
		frames();
	}
	void button(bool down, int which = 0) {
		ImGui::GetIO().AddMouseButtonEvent(which, down);
		frames();
	}
	void key(ImGuiKey key, bool down) {
		ImGui::GetIO().AddKeyEvent(key, down);
		frames();
	}
	// A click at a point: moved there, pressed, released.
	void click(ImVec2 at) {
		mouse(at.x, at.y);
		button(true);
		button(false);
	}
	// The mouse off every window (no tooltip in a logged frame).
	void away() { mouse(-1000.0f, -1000.0f); }
	// A button (or any item) pressed through ImGui's own activation, by its id.
	void activate(ImGuiID id) {
		ImGui::ActivateItemByID(id);
		frames(2);
	}
	// A key chord: the keys pressed in order, released in reverse; the requests it raised.
	std::vector<EditorRequest> chord(std::initializer_list<ImGuiKey> keys) {
		for (const ImGuiKey k : keys) key(k, true);
		for (auto k = std::rbegin(keys); k != std::rend(keys); ++k) key(*k, false);
		return drain();
	}
	static ImGuiID window_id(const char *title) {
		ImGuiWindow *window = ImGui::FindWindowByName(title);
		return window ? window->ID : 0;
	}
};

// An item's id as ImGui makes it: the window's, then each pushed step.
inline ImGuiID item_id(ImGuiID seed, std::initializer_list<const char *> steps) {
	for (const char *step : steps) seed = ImHashStr(step, 0, seed);
	return seed;
}
inline ImGuiID pushed(ImGuiID seed, int value) { return ImHashData(&value, sizeof(value), seed); }

// A popup's item as ImGui names it: the popup window "##Popup_<its id>", then the label.
inline ImGuiID popup_item(ImGuiID popup, const char *label) {
	char name[32];
	std::snprintf(name, sizeof(name), "##Popup_%08x", popup);
	return ImHashStr(label, 0, ImHashStr(name));
}

// The main menu bar's items: its window, then the scope BeginMenuBar pushes.
inline ImGuiID menu_bar_id() { return item_id(Ui::window_id("##MainMenuBar"), {"##menubar"}); }

// A menu of the menu bar opened and an item of it chosen (after a submenu, the submenu's
// item); the requests that raised. A menu left open (its item disabled) is closed.
inline std::vector<EditorRequest> choose(Ui &ui, const char *menu, std::initializer_list<const char *> items) {
	ui.activate(item_id(menu_bar_id(), {menu}));
	int depth = 0;
	for (const char *item : items) {
		char popup[16];
		std::snprintf(popup, sizeof(popup), "##Menu_%02d", depth++);
		ui.activate(item_id(ImHashStr(popup), {item}));
	}
	ImGui::ClosePopupsExceptModals();
	ui.frames(2);
	return ui.drain();
}

// The Problems window's grouping picked from its Group list ("None", "File", "Kind"): it
// groups by kind until one is picked.
inline void problems_grouping(Ui &ui, const char *choice) {
	ui.activate(item_id(Ui::window_id("Problems"), {"Group"}));
	ui.activate(item_id(ImHashStr("##Combo_00"), {choice}));
}

// The one request of a kind among `requests`; null for none or several.
inline const EditorRequest *only(const std::vector<EditorRequest> &requests, EditorRequestKind kind) {
	const EditorRequest *found = nullptr;
	int count = 0;
	for (const EditorRequest &request : requests)
		if (request.kind == kind) {
			found = &request;
			++count;
		}
	return count == 1 ? found : nullptr;
}

// The request when `requests` is exactly one of `kind`.
inline const EditorRequest *one(const std::vector<EditorRequest> &requests, EditorRequestKind kind) {
	return requests.size() == 1 && requests[0].kind == kind ? &requests[0] : nullptr;
}

// The one edit an EditRecord the windows raise carries (a batch of one); an empty edit (a Set of
// no field) for a batch of another size, so a check on it fails rather than reads past the batch.
inline const Edit &edit_of(const EditorRequest &request) {
	static const Edit none;
	return request.edits.size() == 1 ? request.edits.front() : none;
}

// What a frame of the workspace writes as text (ImGui's log), for the lines a window shows.
inline std::string logged_frame(EditorWindows &windows, uint64_t index) {
	ImGui::NewFrame();
	ImGui::LogToBuffer();
	windows.draw_frame(index);
	const std::string text = GImGui->LogBuffer.c_str();
	ImGui::LogFinish();
	ImGui::Render();
	return text;
}
inline std::string logged_frame(Ui &ui) {
	if (ui.pump) ui.pump();
	return logged_frame(ui.windows, ++ui.index);
}

// Every window drawn in the last frame whose content is wider than what shows of it while it
// does not scroll sideways (its content size against its content region), and every cell of
// a table whose content runs past its column (a table that scrolls sideways moves its
// columns, but each still clips its cells): by name, with the widths, for the failure to
// list. A child ImGui makes for a widget is left to the widget (a multiline text box scrolls
// its text itself; the child is named after its "##" label).
inline std::vector<std::string> overflowing() {
	std::vector<std::string> out;
	ImGuiContext &g = *GImGui;
	char line[320];
	for (const ImGuiWindow *window : g.Windows) {
		if (!window->Active || window->Hidden || window->SkipItems) continue;
		if (window->Flags & (ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_HorizontalScrollbar |
		                     ImGuiWindowFlags_AlwaysHorizontalScrollbar))
			continue;
		const char *child = std::strrchr(window->Name, '/');
		if ((window->Flags & ImGuiWindowFlags_ChildWindow) && child && std::strncmp(child + 1, "##", 2) == 0) continue;
		const float visible = window->ContentRegionRect.GetWidth();
		if (window->ContentSize.x > visible + 1.0f) {
			std::snprintf(line, sizeof(line), "window %s: content %.0f wide in %.0f", window->Name, window->ContentSize.x, visible);
			out.push_back(line);
		}
	}
	for (int i = 0; i < g.Tables.GetMapSize(); ++i) {
		const ImGuiTable *table = g.Tables.TryGetMapData(i);
		if (!table || table->LastFrameActive != g.FrameCount) continue;
		if (!table->OuterWindow || table->OuterWindow->SkipItems) continue;
		for (int c = 0; c < table->ColumnsCount; ++c) {
			const ImGuiTableColumn &column = table->Columns[c];
			if (!column.IsEnabled || !column.IsVisibleX) continue;
			const float content = std::max(column.ContentMaxXFrozen, column.ContentMaxXUnfrozen);
			if (content > column.WorkMaxX + 1.0f) {
				std::snprintf(line, sizeof(line), "table %08x in %s, column %d: content to %.0f, past %.0f", table->ID,
				              table->OuterWindow->Name, c, content, column.WorkMaxX);
				out.push_back(line);
			}
		}
	}
	return out;
}

// Whether `text` holds each of `parts`, in that order.
inline bool in_order(const std::string &text, std::initializer_list<const char *> parts) {
	size_t at = 0;
	for (const char *part : parts) {
		const size_t found = text.find(part, at);
		if (found == std::string::npos) return false;
		at = found + std::strlen(part);
	}
	return true;
}

inline size_t count_of(const std::string &text, const char *part) {
	size_t count = 0;
	for (size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + 1)) ++count;
	return count;
}

// The id of an open document's tab in the Document window's tab bar: its label's part after
// "###", its path.
inline ImGuiID document_tab_id(const std::string &path) {
	const ImGuiID bar = item_id(Ui::window_id("Document"), {"documents"});
	return ImHashStr(("###" + path).c_str(), 0, bar);
}

// Two root windows; MAIN holds BACK (a HOTKEY, two ACTIONs, a SOUND), PANEL (holding the
// list CHOICES, whose scrollbar part holds INPART) and TITLE.
inline constexpr const char *kMenu =
        "<SCREEN>\r\n"
        "\t<NAME>OPTIONS</NAME>\r\n"
        "\t<WINDOW type=\"window\" name=\"MAIN\">\r\n"
        "\t\t<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>\r\n"
        "\t\t<WINDOW type=\"button\" name=\"BACK\">\r\n"
        "\t\t\t<HOTKEY>B</HOTKEY>\r\n"
        "\t\t\t<ACTION type=\"POP_SCREEN\"></ACTION>\r\n"
        "\t\t\t<ACTION type=\"WINDOW\" state=\"SHOW\">PANEL</ACTION>\r\n"
        "\t\t\t<APPEARANCE state=\"default\"></APPEARANCE>\r\n"
        "\t\t\t<SOUND state=\"mousein\" trigger=\"MOUSE_OVER\">menu.lwf</SOUND>\r\n"
        "\t\t\t<POSITION><LEFT>10</LEFT><TOP>10</TOP></POSITION>\r\n"
        "\t\t</WINDOW>\r\n"
        "\t\t<WINDOW type=\"window\" name=\"PANEL\">\r\n"
        "\t\t\t<APPEARANCE state=\"default\"></APPEARANCE>\r\n"
        "\t\t\t<WINDOW type=\"list\" name=\"CHOICES\">\r\n"
        "\t\t\t\t<APPEARANCE state=\"default\"></APPEARANCE>\r\n"
        "\t\t\t\t<ITEMS><ITEM value=\"1\">One</ITEM><ITEM value=\"2\">Two</ITEM></ITEMS>\r\n"
        "\t\t\t\t<SCROLLBAR><APPEARANCE state=\"default\"></APPEARANCE>"
        "<WINDOW type=\"static\" name=\"INPART\"><APPEARANCE state=\"default\"></APPEARANCE></WINDOW></SCROLLBAR>\r\n"
        "\t\t\t</WINDOW>\r\n"
        "\t\t</WINDOW>\r\n"
        "\t\t<WINDOW type=\"static\" name=\"TITLE\"><APPEARANCE state=\"default\"></APPEARANCE><STRING>Options</STRING></WINDOW>\r\n"
        "\t</WINDOW>\r\n"
        "\t<WINDOW type=\"window\" name=\"OVERLAY\"><APPEARANCE state=\"default\"></APPEARANCE></WINDOW>\r\n"
        "</SCREEN>\r\n";

// kMenu written as `file` in the fixture's folder and loaded as the project's `relative`.
inline std::shared_ptr<MnuDocument> menu_at(const editor_test::TempProjectDir &dir, const char *file, const char *relative) {
	auto document = std::make_shared<MnuDocument>();
	Diagnostic error;
	CHECK(editor_test::write_text(dir.file(file), kMenu), "menu fixture");
	CHECK(document->load(dir.file(file), relative, AssetKind::Menu, "jo", error), error.message.c_str());
	return document;
}

inline std::shared_ptr<MnuDocument> load_menu(const editor_test::TempProjectDir &dir) {
	std::shared_ptr<MnuDocument> document = menu_at(dir, "options.mnu", "options.mnu");
	CHECK(!document->blocked() && document->issues().empty() && document->rows().size() == 1, "the menu reads clean");
	return document;
}

inline NodeAddress named(const Document &document, const char *name) {
	NodeAddress address;
	find_definition(AssetGraph(), document, name, address);
	return address;
}

inline AssetEntry file_entry(const std::string &name, const std::string &path, AssetKind kind) {
	AssetEntry entry;
	entry.logical_name = name;
	entry.relative_path = path;
	entry.kind = kind;
	return entry;
}

// An open project with one menu, the active document.
inline SessionView menu_view(const std::shared_ptr<MnuDocument> &document) {
	SessionView v;
	v.project.open = true;
	v.project.root = "C:/mods/Menus";
	editor_test::own(v.project.document).title = "Menus";
	editor_test::own(v.project.scan)
			.entries.push_back(file_entry("options.mnu", document->path(), AssetKind::Menu));
	editor_test::own(v.project.scan).index();
	v.documents.open.push_back(document);
	v.documents.active = document->path();
	return v;
}

inline void select_in(SessionView &v, const NodeAddress &address) {
	v.documents.selection.primary = address;
	v.documents.selection.records = {address};
	v.revisions.touch(ViewConcern::Selection);
}

// A view put in the place of another, at its address (which a window's cache knows it by): its
// counters and its events carry on from the old view's, every concern moved.
inline void replace_view(SessionView &v, SessionView fresh) {
	const ViewRevisions revisions = v.revisions;
	const ViewEvents events = v.events;
	v = std::move(fresh);
	v.revisions = revisions;
	v.events = events;
	for (size_t concern = 0; concern < kViewConcernCount; ++concern)
		v.revisions.touch(static_cast<ViewConcern>(concern));
}

// An event posted into a hand-made view as the session posts it (view_events.h), with the
// concern its site moves.
inline void post_event(SessionView &v, ViewEventKind kind, const std::string &path = std::string(),
		const NodeAddress &address = NodeAddress(), const std::string &field = std::string(),
		bool flag = false, uint64_t tag = 0) {
	ViewEvent event;
	event.kind = kind;
	event.path = path;
	event.address = address;
	event.field = field;
	event.flag = flag;
	event.tag = tag;
	v.events.post(std::move(event));
	const bool selection = kind == ViewEventKind::RevealRecord || kind == ViewEventKind::RevealFile;
	v.revisions.touch(selection ? ViewConcern::Selection : ViewConcern::Dialogs);
}

// The newest event of `kind` a view holds (seq 0: none).
inline ViewEvent newest_event(const SessionView &v, ViewEventKind kind) {
	for (auto event = v.events.held().rbegin(); event != v.events.held().rend(); ++event)
		if (event->kind == kind)
			return *event;
	return ViewEvent();
}

using editor_test::NoProcess;

// A viewport's device as the Shell's stands in a test: its picture an invisible button where the
// Shell's SubViewport image goes (what ImGuiGD draws), at the size the canvas draws it, the origin
// kept (where a design unit or a marker's pixel is: the picture's corner the canvas hands it, which
// is where the canvas's cursor stands) and the canvas's clip; what its viewport asked of it, in order.
struct DrawnDevice final : ViewportDevice {
	ImVec2 origin;
	int width = 0, height = 0;
	int draws = 0;
	bool drawn = false; // a canvas drew it since the last pump
	ViewportPicture last;
	std::vector<ViewportAction> taken;
	void draw(const ViewportPicture &picture) override {
		drawn = true;
		const ImVec2 cursor = ImGui::GetCursorScreenPos();
		origin = ImVec2(picture.x, picture.y);
		CHECK(cursor.x == picture.x && cursor.y == picture.y, "the picture where the canvas's cursor stands");
		width = picture.width;
		height = picture.height;
		last = picture;
		++draws;
		ImGui::InvisibleButton("godot_subviewport", ImVec2(float(picture.width), float(picture.height)));
	}
	// Its size as the Shell's device reports it: the one a canvas drew it at since the last pump, else
	// its viewport's state's.
	void take(ViewportAction action, const ViewportModel &model, const SessionView &, const PreviewClock &,
			ViewportDeviceReport &report) override {
		taken.push_back(action);
		report.width = drawn ? width : model.state().width;
		report.height = drawn ? height : model.state().height;
		report.canvas_sized = drawn && last.canvas_sized;
		drawn = false;
	}
	void tick(const ViewportModel &, const PreviewClock &) override {}
};

// The Shell's devices in a test (the windows' device source, set_devices): one DrawnDevice per
// (document, kind) the cache holds, every one it made kept by its address.
struct DrawnDevices {
	std::vector<DrawnDevice *> made;
	ViewportDeviceCache cache{ [this](ViewportKind) {
		auto device = std::make_unique<DrawnDevice>();
		made.push_back(device.get());
		return std::unique_ptr<ViewportDevice>(std::move(device));
	} };
	// The Shell's pump after the session's poll: the devices follow the viewports.
	void sync(Viewports &viewports, const SessionView &view) { cache.sync(viewports, view); }
	DrawnDevice *held(const std::string &path, ViewportKind kind) const {
		return static_cast<DrawnDevice *>(cache.held(path, kind));
	}
};

// The viewports a hand-made view shares, kept as the session keeps its own (DocumentsView::
// viewports): with the project's files an empty project's (a menu's compile reads none: its fonts
// and textures missing, its rects the game's), tracked to the view and followed by their devices at
// each pump, and a SetViewport the windows raise applied as the session serves it (the windows'
// other requests kept for the test to drain).
struct HandViewports {
	std::shared_ptr<Viewports> viewports = std::make_shared<Viewports>();
	DrawnDevices devices;
	void bind(SessionView &v) {
		v.documents.viewports = viewports;
		if (!v.findings.assets) v.findings.assets = std::make_shared<const ProjectAssetSource>();
	}
	// A SetViewport's change applied to the viewport at `path` (the session's set_viewport).
	bool set(const SessionView &v, const std::string &path, const char *change) {
		opennova::io::JsonValue json;
		std::string error;
		return opennova::io::json_parse(change, json, error) && viewports->set(v, path, json, error);
	}
	// The Shell's pump: the windows' SetViewports served, the viewports tracked to the view (as the
	// session does after each change) and their devices synced.
	void pump(EditorWindows &windows, SessionView &v) {
		std::vector<EditorRequest> kept;
		EditorRequest request;
		while (windows.take_request(request)) {
			if (request.kind == EditorRequestKind::SetViewport) set(v, request.path, request.viewport.c_str());
			else kept.push_back(std::move(request));
		}
		for (EditorRequest &held : kept) windows.request(std::move(held));
		viewports->track(v);
		devices.sync(*viewports, v);
	}
	const ViewportModel *find(const std::string &path, ViewportKind kind) const { return viewports->find(path, kind); }
	DrawnDevice *device(const std::string &path, ViewportKind kind) const { return devices.held(path, kind); }
};

// A project for the Preview window: a new project's files (main.mnu among them), a skinned
// model and its clips from the Blender add-on's scene texts with a catalog item pairing the
// two, and armory.3di (user points).
inline bool preview_project(ProjectSession &session, const editor_test::TempProjectDir &dir) {
	if (!session.handle(request::new_project(dir.file("project"), "Preview"))) return false;
	session.run_operations();
	editor_test::create_missing_files(session);
	const SessionView &v = session.view();
	const std::string repo = test_paths_repo_root(__FILE__);
	const std::string source = dir.file("source");
	if (!editor_test::write_bytes(source + "/skinned.o3d", test_io::read_file(repo + "/fixtures/threedi/o3d/skinned.o3d")) ||
	    !editor_test::write_text(source + "/skin.o3a", editor_test::kSkinClips))
		return false;
	EditorRequest import = request::of(EditorRequestKind::ImportFiles);
	import.imports = {{source + "/skinned.o3d", {}}, {source + "/skin.o3a", {}}};
	session.handle(import);
	session.run_operations();
	if (!editor_test::write_text(v.project.root + "/defs/items.def",
	                             "begin \"Skinned Thing\"\nid 100200\ntype building\ngraphic skinned\nanim_def skin\nend\n") ||
	    !editor_test::write_bytes(v.project.root + "/models/armory.3di",
	                              test_io::read_file(repo + "/fixtures/threedi/synth/armory.3di")))
		return false;
	session.handle(request::rescan());
	session.run_operations();
	return v.project.scan->find("skinned.3di") && v.project.scan->find("SKIN.adm") && v.project.scan->find("walk.bad") && v.project.scan->find("armory.3di");
}

// An import dialog's preview (S11g), as a session plans it: menu<stretch>.mnu chosen from
// `folder`, and walk.o3a whose two outputs come together; the font and the texture the menu
// needs found beside it (the font in the game install too, the two files differing); a texture
// whose name the archives cannot store, found but not taken; a texture found nowhere; a screen
// reference and a terrain not followed; the cap reached; a file that could not be read.
// `stretch` makes every name the dialog shows run long.
inline DialogsView::ImportPreview planned_import(const std::string &folder, const std::string &stretch = std::string()) {
	using State = ImportPlanRow::State;
	DialogsView::ImportPreview preview;
	preview.open = true;
	preview.with_dependencies = true;
	const std::string menu = "menu" + stretch + ".mnu";
	const std::string found_in = "the folder " + folder;
	const auto row = [&](State state, const std::string &name, AssetKind kind, const ImportSource &source,
	                     const std::string &destination) {
		ImportPlanRow out;
		out.state = state;
		out.selected = state != State::NotFound;
		out.source = source;
		out.name = name;
		out.kind = kind;
		out.destination = destination;
		out.found_in = state == State::NotFound ? std::string() : found_in;
		return out;
	};
	const ImportSource chosen{folder + "/" + menu, "", false, false};
	const ImportSource clips{folder + "/walk.o3a", "", false, false};
	const auto beside = [&folder](const std::string &name) { return ImportSource{folder + "/" + name, "", false, true}; };
	preview.roots = {chosen, clips};
	ImportPlanRow table = row(State::Selected, "CHECK.adm", AssetKind::AnimationMap, clips, "anims/CHECK.adm");
	ImportPlanRow clip = row(State::Selected, "walk.bad", AssetKind::Animation, clips, "anims/walk.bad");
	table.made_from = clip.made_from = "walk.o3a";
	ImportPlanRow font = row(State::Found, "arial99.fnt", AssetKind::Font, beside("arial99.fnt"), "fonts/arial99.fnt");
	font.needed_by = {menu, "MAIN/TITLE" + stretch, "font.name", ReferenceKind::Font, "arial99", -1};
	ImportRival rival;
	rival.name = "ARIAL99.FNT";
	rival.found_in = "the game install";
	rival.source = {"C:/Games/Joint Operations", "ARIAL99.FNT", true, false};
	rival.differs = true;
	font.rivals = {rival};
	ImportPlanRow gone = row(State::NotFound, "gone" + stretch + ".tga", AssetKind::Texture, ImportSource(), std::string());
	gone.needed_by = {menu, "MAIN/KEEP/Appearance 1", "value", ReferenceKind::MenuTexture, "gone" + stretch + ".tga", -1};
	ImportPlanRow logo = row(State::Found, "logo.tga", AssetKind::Texture, beside("logo.tga"), "logo.tga");
	logo.needed_by = {menu, "MAIN/LOGO/Appearance 1", "value", ReferenceKind::MenuTexture, "logo.tga", -1};
	ImportPlanRow cut = row(State::Found, "a_long_texture_name.tga", AssetKind::Texture, beside("a_long_texture_name.tga"),
	                        "a_long_texture_name.tga");
	cut.needed_by = {menu, "MAIN/BADGE/Appearance 1", "value", ReferenceKind::MenuTexture, "a_long_texture_name.tga", -1};
	cut.selected = false;
	cut.problem = "a_long_texture_name.tga is longer than the 16 characters the game's archives store.";
	ImportPlan plan;
	plan.rows = { row(State::Selected, menu, AssetKind::Menu, chosen, "menus/" + menu), table, clip,
		font, gone, logo, cut };
	plan.not_followed = {{ReferenceKind::MenuScreen, AssetKind::Unknown, 1, menu},
	                     {ReferenceKind::None, AssetKind::Terrain, 1, "level" + stretch + ".trn"}};
	plan.truncated = true;
	plan.diagnostics = { editor_test::finding_of(DiagnosticSeverity::Warning, "import.unreadable",
			"The file could not be read" + stretch + ". The files it names are not looked for.",
			"broken.mnu") };
	preview.plan = std::make_shared<const ImportPlan>(std::move(plan));
	return preview;
}

// The import dialog's body (a child window: the lists that scroll), whose id its items'
// ids start from, and an item of a table in it by the row's index.
inline ImGuiID import_body_id() {
	const ImGuiID dialog = ImHashStr("Import files");
	char name[64];
	std::snprintf(name, sizeof(name), "Import files/import_body_%08X", item_id(dialog, {"import_body"}));
	return ImHashStr(name);
}
inline ImGuiID import_table_item(const char *table, int index, const char *label) {
	return item_id(pushed(item_id(import_body_id(), {table}), index), {label});
}

// The workspace's tests (workspace_test.cpp): the six-window shell of S11d.
void run_workspace_tests();
// The change marks, the reveal, Output and the shortcuts (markers_test.cpp), and the
// bounds sweep (bounds_test.cpp): S11e.
void run_marker_tests();
void run_bounds_tests();
// The field controls (field_widgets_test.cpp): S12 D4.
void run_field_widget_tests();
// The reference picker (reference_picker_test.cpp): S12 D7.
void run_reference_picker_tests();
// Find in a document and in the project (find_test.cpp): S12 D8.
void run_find_tests();
// Rename everywhere from the Inspector (rename_test.cpp): S12 D9.
void run_rename_tests();
// Every control that raises a request enabled exactly when the busy gate takes it
// (gate_test.cpp): S13 A1.
void run_gate_tests();

} // namespace editor_ui_test
