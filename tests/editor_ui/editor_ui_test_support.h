// Shared scaffolding for the editor_ui ctest (editor_windows_test.cpp, workspace_test.cpp,
// markers_test.cpp, bounds_test.cpp, field_widgets_test.cpp, reference_picker_test.cpp,
// find_test.cpp, rename_test.cpp, gate_test.cpp): the failure counter and CHECK, a null ImGui backend
// (a display size and a built font atlas, no platform or renderer), the workspace driven
// frame by frame with the mouse and the keys (Ui), the ids ImGui gives items, what a frame
// writes as text, the requests a test looks for, the menu fixture the windows are driven
// over, the preview devices a test stands in for the shell's, and a project with every
// preview's files.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <editor/documents/mnu_document.h>
#include <editor/graph/reference_queries.h>
#include <editor/preview/menu_screen_render.h>
#include <editor/preview/model_preview_state.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/session_view.h>
#include <editor/ui/editor_windows.h>
#include <editor/ui/menu_preview_pane.h>
#include <editor/ui/model_preview_pane.h>
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

// The workspace over a 1920x1080 display unless a test sizes it, driven frame by frame.
struct Ui {
	NullBackend backend;
	EditorWindows windows;
	uint64_t index = 0;
	Ui() {
		ImGui::GetIO().DisplaySize = ImVec2(1920.0f, 1080.0f);
		windows.pass().attach_imgui(backend.context, test_alloc, test_free, nullptr);
	}
	~Ui() { windows.pass().detach_imgui(); }
	void frames(int count = 1) {
		for (int i = 0; i < count; ++i) frame(windows, ++index);
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
inline std::string logged_frame(Ui &ui) { return logged_frame(ui.windows, ++ui.index); }

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
	v.project_open = true;
	v.project_root = "C:/mods/Menus";
	v.document.title = "Menus";
	v.scan.entries.push_back(file_entry("options.mnu", document->path(), AssetKind::Menu));
	v.scan.index();
	v.documents.push_back(document);
	v.active_document = document->path();
	return v;
}

inline void select_in(SessionView &v, const NodeAddress &address) {
	v.selection = address;
	v.selected = {address};
	v.revisions.touch(ViewConcern::Selection);
}

// A view put in the place of another, at its address (which a window's cache knows it by): its
// counters carry on from the old view's, every concern moved.
inline void replace_view(SessionView &v, SessionView fresh) {
	const ViewRevisions revisions = v.revisions;
	v = std::move(fresh);
	v.revisions = revisions;
	for (size_t concern = 0; concern < kViewConcernCount; ++concern)
		v.revisions.touch(static_cast<ViewConcern>(concern));
}

using editor_test::NoProcess;

// No file at all: a headless menu render with nothing mounted.
struct NoFiles : opennova::FileSource {
	bool read(const std::string &, std::vector<uint8_t> &) const override { return false; }
	uint64_t stamp(const std::string &) const override { return 0; }
};

// A device over the engine's headless render, drawing an invisible button where the
// shell's SubViewport image goes (what ImGuiGD draws), and remembering where.
class FakePreview : public MenuPreviewViewport {
public:
	MenuScreenRender render;
	NoFiles files;
	MenuPreviewOptions held;
	MenuPreviewStatus shown_status = MenuPreviewStatus::Ready;
	std::string shown_detail;
	bool stale = false; // the picture of another revision than the document's
	ImVec2 origin;
	int width = 0, height = 0;

	MenuPreviewStatus status(std::string *detail) const override {
		if (detail) *detail = shown_detail;
		return shown_status;
	}
	const std::vector<std::string> &missing() const override { return none_; }
	const std::vector<std::string> &unreadable() const override { return none_; }
	void draw(int device_width, int device_height) override {
		origin = ImGui::GetCursorScreenPos();
		width = device_width;
		height = device_height;
		ImGui::InvisibleButton("godot_subviewport", ImVec2(float(device_width), float(device_height)));
	}
	void set_options(const MenuPreviewOptions &options) override { held = options; }
	const MenuPreviewOptions &options() const override { return held; }
	const opennova::menu::MenuFrameCompiler *compiler() const override {
		return shown_status == MenuPreviewStatus::Ready ? &render.compiler() : nullptr;
	}
	const opennova::menu::MenuFrameState *frame_state() const override {
		return shown_status == MenuPreviewStatus::Ready ? &render.state() : nullptr;
	}
	uint64_t shown_revision() const override { return render.revision() + (stale ? 1 : 0); }

private:
	std::vector<std::string> none_;
};

// The model pane's device over the real portable half: the test's follow() is the shell's
// refresh; the picture an invisible button where the shell draws its texture, at the size
// the pane asks for, the origin kept (where a marker's pixel is).
struct ModelDevice : ModelPreviewViewport {
	ModelPreviewModel held;
	ImVec2 origin;
	ModelPreviewModel &model() override { return held; }
	void draw(int device_width, int device_height) override {
		origin = ImGui::GetCursorScreenPos();
		held.set_device_size(device_width, device_height);
		ImGui::InvisibleButton("godot_subviewport", ImVec2(float(device_width), float(device_height)));
	}
};

// A project for the Preview window: a new project's files (main.mnu among them), a skinned
// model and its clips from the Blender add-on's scene texts with a catalog item pairing the
// two, and armory.3di (user points).
inline bool preview_project(ProjectSession &session, const editor_test::TempProjectDir &dir) {
	if (!session.handle(request::new_project(dir.file("project"), "Preview"))) return false;
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
	if (!editor_test::write_text(v.project_root + "/defs/items.def",
	                             "begin \"Skinned Thing\"\nid 100200\ntype building\ngraphic skinned\nanim_def skin\nend\n") ||
	    !editor_test::write_bytes(v.project_root + "/models/armory.3di",
	                              test_io::read_file(repo + "/fixtures/threedi/synth/armory.3di")))
		return false;
	session.handle(request::rescan());
	return v.scan.find("skinned.3di") && v.scan.find("SKIN.adm") && v.scan.find("walk.bad") && v.scan.find("armory.3di");
}

// An import dialog's preview (S11g), as a session plans it: menu<stretch>.mnu chosen from
// `folder`, and walk.o3a whose two outputs come together; the font and the texture the menu
// needs found beside it (the font in the game install too, the two files differing); a texture
// whose name the archives cannot store, found but not taken; a texture found nowhere; a screen
// reference and a terrain not followed; the cap reached; a file that could not be read.
// `stretch` makes every name the dialog shows run long.
inline SessionView::ImportPreview planned_import(const std::string &folder, const std::string &stretch = std::string()) {
	using State = ImportPlanRow::State;
	SessionView::ImportPreview preview;
	preview.open = true;
	preview.with_dependencies = true;
	preview.serial = 1;
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
	preview.plan.rows = {row(State::Selected, menu, AssetKind::Menu, chosen, "menus/" + menu), table, clip, font, gone, logo, cut};
	preview.plan.not_followed = {{ReferenceKind::MenuScreen, AssetKind::Unknown, 1, menu},
	                             {ReferenceKind::None, AssetKind::Terrain, 1, "level" + stretch + ".trn"}};
	preview.plan.truncated = true;
	preview.plan.diagnostics = {make_diagnostic(DiagnosticSeverity::Warning, "import.unreadable",
	                                            "The file could not be read" + stretch + ". The files it names are not looked for.",
	                                            "broken.mnu")};
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
