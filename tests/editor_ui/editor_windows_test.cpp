// The editor's workspace over a null ImGui backend (ADR 0046 d11): the windows and
// their placements, a layout pass from a seeded view produces draw data, the request
// queue round-trips, the pickers land where they were asked for, and the menu bar
// carries the editor's menus before the pass's own.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <editor/session/session_view.h>
#include "../editor/editor_test_support.h"
#include <editor/ui/editor_windows.h>

#include <imgui.h>

using namespace opennova::editor;
namespace devtools = opennova::devtools;

namespace {

int g_failures = 0;

#define CHECK(cond, message)                                                          \
	do {                                                                              \
		if (!(cond)) {                                                                \
			std::printf("FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, message, #cond); \
			++g_failures;                                                             \
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

void *test_alloc(size_t size, void *) { return std::malloc(size); }
void test_free(void *ptr, void *) { std::free(ptr); }

bool frame(EditorWindows &windows, uint64_t index) {
	ImGui::NewFrame();
	const bool drew = windows.draw_frame(index);
	ImGui::Render();
	return drew;
}

const devtools::Window *find_window(const devtools::ImGuiPass &pass, const char *title) {
	for (int i = 0; i < pass.window_count(); ++i) {
		if (std::strcmp(pass.window(i).title(), title) == 0) return &pass.window(i);
	}
	return nullptr;
}

SessionView seeded_view() {
	SessionView v;
	v.revision = 7;
	v.project_open = true;
	v.project_root = "C:/mods/My Game";
	v.document.title = "My Game";
	AssetEntry entry;
	entry.logical_name = "main.mnu";
	entry.relative_path = "menus/main.mnu";
	entry.kind = AssetKind::Menu;
	entry.size_bytes = 2048;
	v.scan.entries.push_back(entry);
	RequirementRow row;
	row.role = "main_menu";
	row.name = "main.mnu";
	row.required = true;
	row.state = RequirementState::Present;
	v.requirements.rows.push_back(row);
	RequirementRow missing;
	missing.role = "gametext";
	missing.name = "gametext.bin";
	missing.required = true;
	missing.state = RequirementState::Missing;
	v.requirements.rows.push_back(missing);
	v.requirements.required_total = 2;
	v.requirements.required_missing = 1;
	v.diagnostics.push_back(make_diagnostic(DiagnosticSeverity::Error, "requirement.missing",
	                                        "The game needs gametext.bin and the project has no such file."));
	v.output = {"Opened My Game", "Build started."};
	v.recent_projects = {"C:/mods/My Game", "C:/mods/Other"};
	v.status = "Opened My Game.";
	return v;
}

void test_windows_and_layout() {
	NullBackend backend;
	EditorWindows windows;
	const devtools::ImGuiPass &pass = windows.pass();
	CHECK(pass.window_count() == 7, "seven windows");
	const devtools::Window *project = find_window(pass, "Project");
	CHECK(project != nullptr && !project->is_closeable(), "Project is the home window and never closes");
	CHECK(project != nullptr && project->initial_dock_placement() == devtools::InitialDockPlacement::Center,
	      "Project sits in the centre");
	const devtools::Window *needs = find_window(pass, "Files the game needs");
	CHECK(needs != nullptr && needs->initial_dock_placement() == devtools::InitialDockPlacement::Right,
	      "the checklist docks right");
	const devtools::Window *files = find_window(pass, "Project files");
	CHECK(files != nullptr && files->initial_dock_placement() == devtools::InitialDockPlacement::Left,
	      "the file list docks left");
	CHECK(find_window(pass, "Problems") != nullptr && find_window(pass, "Output") != nullptr,
	      "Problems and Output exist");
	CHECK(find_window(pass, "Output")->initial_dock_placement() == devtools::InitialDockPlacement::Bottom,
	      "Output docks along the bottom");
	CHECK(pass.is_open(), "the workspace is open from construction");

	// Unattached: nothing draws. Attached: a pass with no project draws the home form.
	CHECK(!frame(windows, 1), "no draw before attach");
	CHECK(windows.pass().attach_imgui(backend.context, test_alloc, test_free, nullptr), "attach");
	CHECK(frame(windows, 2), "the home layout draws");
	CHECK(ImGui::GetDrawData() != nullptr && ImGui::GetDrawData()->TotalVtxCount > 0, "draw data");

	// A seeded open project draws every window's table.
    SessionView v = seeded_view();
    editor_test::TempProjectDir dir("opennova_catalog_ui_test");
    CHECK(editor_test::write_text(dir.file("items.def"), "begin \"Marker\"\nid 100001\ntype marker\nend\n"), "catalog fixture");
    auto document = std::make_shared<EditableDocument>(); Diagnostic error;
    CHECK(document->load(dir.file("items.def"), "items.def", AssetKind::ItemDefs, "jo", error), "catalog loads");
    v.documents.push_back(document); v.active_document = document->path();
    v.selection = {document->rows()[0]->id, opennova::def::DefRecordKind::Item, 0};
	windows.set_view(&v);
	CHECK(frame(windows, 3), "the project layout draws");
	CHECK(ImGui::GetDrawData()->TotalVtxCount > 0, "draw data with a project");
	CHECK(windows.pending_requests() == 0, "drawing raises no request by itself");
	for (uint64_t i = 4; i < 8; ++i) frame(windows, i); // the dock layout settles
	CHECK(windows.pending_requests() == 0, "still none");
	windows.pass().detach_imgui();
}

void test_requests_round_trip() {
	EditorWindows windows;
	windows.request(make_request(EditorRequestKind::OpenProject, "C:/mods/A"));
	EditorRequest set = make_request(EditorRequestKind::SetFeature, std::string(), "mission");
	set.flag = true;
	windows.request(set);
	CHECK(windows.pending_requests() == 2, "two queued");
	EditorRequest out;
	CHECK(windows.take_request(out) && out.kind == EditorRequestKind::OpenProject && out.path == "C:/mods/A",
	      "oldest first");
	CHECK(windows.take_request(out) && out.kind == EditorRequestKind::SetFeature && out.text == "mission" && out.flag,
	      "then the feature toggle");
	CHECK(!windows.take_request(out), "drained");

	// The pickers' answers: an open-project pick becomes the request, a runtime pick
	// the setting, a cancelled pick nothing.
	windows.deliver_pick(PickPurpose::OpenProject, "C:/mods/B");
	CHECK(windows.take_request(out) && out.kind == EditorRequestKind::OpenProject && out.path == "C:/mods/B",
	      "open pick");
	windows.deliver_pick(PickPurpose::RuntimeExecutable, "C:/tools/opennova.exe");
	CHECK(windows.take_request(out) && out.kind == EditorRequestKind::SetRuntimeExecutable &&
	              out.path == "C:/tools/opennova.exe",
	      "runtime pick");
	windows.deliver_pick(PickPurpose::OpenProject, "");
	CHECK(!windows.take_request(out), "a cancelled pick raises nothing");
	windows.deliver_pick(PickPurpose::NewProjectLocation, "C:/mods/New");
	CHECK(!windows.take_request(out), "a location pick fills the form, it does not open");
}

} // namespace

int main() {
	test_windows_and_layout();
	test_requests_round_trip();
	if (g_failures == 0) std::printf("editor_ui: all tests passed\n");
	return g_failures == 0 ? 0 : 1;
}
