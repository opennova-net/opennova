// ONED's run surface against a null ImGui backend: the surface starts open,
// draws a full-viewport window from the seeded fields and pushed state, keeps
// the request queue in order, and the text fields round-trip through the UI.
#include <devtools/oned_ui.h>

#include <imgui.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

using opennova::devtools::OnedAction;
using opennova::devtools::OnedRequest;
using opennova::devtools::OnedStatusKind;
using opennova::devtools::OnedUi;

namespace {

int g_failures = 0;

#define CHECK(cond, message)                                                          \
	do {                                                                              \
		if (!(cond)) {                                                                \
			std::printf("FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, message, #cond); \
			++g_failures;                                                             \
		}                                                                             \
	} while (0)

struct NullBackend {
	ImGuiContext *context = nullptr;

	NullBackend() {
		context = ImGui::CreateContext();
		ImGuiIO &io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.DisplaySize = ImVec2(760.0f, 430.0f);
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

void test_surface_draws_from_the_seeded_state() {
	NullBackend backend;
	OnedUi ui;
	CHECK(ui.pass().is_open(), "the run surface is open from construction");
	CHECK(ui.pass().window_count() == 1, "one full-viewport surface");
	ui.set_resource_dir("C:/games/jo");
	ui.set_game_code("jo");
	ui.set_expansion("jox01");
	ui.set_retail_dir("C:/retail/jo");
	ui.set_recent_dirs({"C:/games/jo", "C:/games/jodemo"});
	ui.set_readiness("", "Select a valid resource directory first.", false);
	ui.set_status("Choose loose or packed game data, then run OpenNova or retail.", OnedStatusKind::INFO);
	CHECK(ui.resource_dir() == "C:/games/jo", "the seeded field reads back");
	CHECK(ui.opennova_block().empty() && !ui.retail_block().empty(), "readiness carried");
	CHECK(!ui.is_running(), "not running");

	ImGui::NewFrame();
	CHECK(!ui.draw_frame(1), "detached: nothing drawn");
	ImGui::Render();
	CHECK(ImGui::GetDrawData()->CmdListsCount == 0, "no draw lists while detached");

	CHECK(ui.pass().attach_imgui(backend.context, &test_alloc, &test_free, nullptr), "the context is adopted");
	ImGui::NewFrame();
	CHECK(ui.draw_frame(2), "attached + open: drawn");
	ImGui::Render();
	CHECK(ImGui::GetDrawData()->CmdListsCount > 0 && ImGui::GetDrawData()->TotalVtxCount > 0,
			"the surface produced draw lists");
	CHECK(!ui.has_requests(), "drawing without input pushes no requests");
	ui.pass().detach_imgui();
}

void test_requests_queue_in_order() {
	OnedUi ui;
	OnedRequest request;
	CHECK(!ui.take_request(request), "empty queue");
	ui.push_request(OnedRequest{OnedAction::SELECT_RECENT, 1});
	ui.push_request(OnedRequest{OnedAction::RUN_OPENNOVA, -1});
	CHECK(ui.has_requests(), "queued");
	CHECK(ui.take_request(request) && request.action == OnedAction::SELECT_RECENT && request.index == 1, "FIFO head");
	CHECK(ui.take_request(request) && request.action == OnedAction::RUN_OPENNOVA, "FIFO tail");
	CHECK(!ui.take_request(request), "drained");
}

void test_status_and_fields_round_trip() {
	OnedUi ui;
	ui.set_status("Could not stop the running process.", OnedStatusKind::ERROR);
	CHECK(ui.status_text() == "Could not stop the running process.", "status text");
	CHECK(ui.status_kind() == OnedStatusKind::ERROR, "status kind");
	ui.set_readiness("", "", true);
	CHECK(ui.is_running(), "running");
	ui.set_retail_dir(" C:/x ");
	CHECK(ui.retail_dir() == " C:/x ", "the UI never trims: the embedder owns the settings semantics");
}

}  // namespace

int main() {
	test_surface_draws_from_the_seeded_state();
	test_requests_queue_in_order();
	test_status_and_fields_round_trip();
	if (g_failures != 0) {
		std::printf("%d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("oned_ui_test: OK\n");
	return 0;
}
