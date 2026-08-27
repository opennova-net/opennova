// The game's dev tools against a null ImGui backend: the context hand-off
// sets the docking/viewport policy, a layout pass with the Stats window open
// produces draw data, the Stats window arms and disarms the board's capture
// on its visibility edges and formats a drained window, and the ImGui ABI
// fingerprint is the pinned one (the imgui-godot addon rejects any other).
#include <devtools/game_dev_tools.h>
#include <devtools/imgui_abi.h>
#include <devtools/stats_window.h>

#include <imgui.h>

#include <cstdio>
#include <cstring>

using opennova::devtools::CaptureWindow;
using opennova::devtools::GameDevTools;
using opennova::devtools::FrameStatsBoard;
using opennova::devtools::Slot;
using opennova::devtools::StatsWindow;

namespace {

int g_failures = 0;

#define CHECK(cond, message)                                                          \
	do {                                                                              \
		if (!(cond)) {                                                                \
			std::printf("FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, message, #cond); \
			++g_failures;                                                             \
		}                                                                             \
	} while (0)

// A headless ImGui frame: the null example's setup (a display size and a
// built font atlas), no platform or renderer backend.
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

int find_row(const StatsWindow &stats, const char *id) {
	for (int i = 0; i < stats.row_count(); ++i) {
		if (std::strcmp(stats.row_id(i), id) == 0) {
			return i;
		}
	}
	return -1;
}

void test_abi_fingerprint_is_the_pinned_one() {
	const opennova::devtools::ImGuiAbi abi = opennova::devtools::imgui_abi();
	CHECK(std::strcmp(abi.version, "1.91.6") == 0, "IMGUI_VERSION is the addon's bundled release");
	CHECK(abi.io_size == static_cast<int>(sizeof(ImGuiIO)), "io size matches this build");
	CHECK(abi.vert_size == 20, "ImDrawVert is pos+uv+col");
	CHECK(abi.idx_size == 2, "16-bit indices");
	CHECK(abi.wchar_size == 2, "16-bit ImWchar");
	// The ImGuiIO layout the addon fingerprints, with IMGUI_DISABLE_OBSOLETE_FUNCTIONS
	// (the three clipboard members gone), as MSVC x64 lays it out. A drift here means the pin moved.
#if defined(_WIN64) || defined(__x86_64__) || defined(__aarch64__)
	CHECK(sizeof(ImGuiIO) == 3008, "sizeof(ImGuiIO) at v1.91.6-docking without obsolete members (64-bit)");
#endif
}

void test_attach_sets_docking_and_viewport_policy() {
	NullBackend backend;
	GameDevTools tools;
	CHECK(!tools.pass().is_attached(), "fresh tools are detached");
	CHECK(!tools.pass().attach_imgui(nullptr, nullptr, nullptr, nullptr), "a null context is refused");
	CHECK(tools.pass().attach_imgui(backend.context, &test_alloc, &test_free, nullptr), "the context is adopted");
	CHECK(tools.pass().is_attached(), "attached after adoption");
	const ImGuiIO &io = ImGui::GetIO();
	CHECK((io.ConfigFlags & ImGuiConfigFlags_DockingEnable) != 0, "docking enabled on attach");
	CHECK((io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0, "multi-viewport requested on attach");
	CHECK(tools.pass().attach_imgui(backend.context, &test_alloc, &test_free, nullptr), "re-attach is harmless (reloadable extension)");
	tools.pass().detach_imgui();
	CHECK(!tools.pass().is_attached(), "detached");
	CHECK(ImGui::GetCurrentContext() == nullptr, "detach drops the engine copy's context binding");
	ImGui::SetCurrentContext(backend.context);
}

void test_layout_pass_draws_the_stats_window_and_gates_capture() {
	NullBackend backend;
	GameDevTools tools;
	FrameStatsBoard board;
	tools.pass().attach_imgui(backend.context, &test_alloc, &test_free, nullptr);
	tools.set_frame_stats(&board);
	CHECK(tools.pass().window_count() == 2, "Stats + demo registered");
	CHECK(tools.stats_window().open, "the Stats window opens by default");
	CHECK(!tools.pass().window(1).open, "the demo window starts closed");
	CHECK(!board.is_capture_active(), "closed tools capture nothing");

	ImGui::NewFrame();
	CHECK(!tools.pass().draw_frame(1), "closed tools draw nothing");
	ImGui::Render();
	CHECK(ImGui::GetDrawData()->CmdListsCount == 0, "no draw lists while closed");

	tools.pass().set_open(true);
	CHECK(board.is_capture_active(), "opening the tools with the Stats window open arms capture");
	board.add(Slot::FRAME_WALL, 16000, 1);
	board.add(Slot::SIM_STEP, 1500, 1);
	board.add(Slot::SIM_TICKS, 1, 1);
	board.add(Slot::FRAME_WALL, 17000, 2);
	board.add(Slot::SIM_STEP, 500, 2);
	board.add(Slot::SIM_TICKS, 1, 2);

	ImGui::NewFrame();
	CHECK(tools.pass().draw_frame(3), "open tools draw");
	ImGui::Render();
	CHECK(ImGui::GetDrawData()->CmdListsCount > 0, "the layout pass produced draw lists");
	CHECK(ImGui::GetDrawData()->TotalVtxCount > 0, "with vertices");

	const StatsWindow &stats = tools.stats_window();
	CHECK(stats.reading_frames() == 3, "the first visible frame drained the board");
	const int frame_row = find_row(stats, "frame");
	const int sim_row = find_row(stats, "sim");
	CHECK(frame_row >= 0 && sim_row >= 0, "the row table carries the frame and sim rows");
	// (16000 + 17000) us over 3 render frames = 11.00 ms mean, 17.00 ms peak.
	CHECK(std::strcmp(stats.row_average(frame_row), "11.00") == 0, "frame mean ms");
	CHECK(std::strcmp(stats.row_peak(frame_row), "17.00") == 0, "frame peak ms");
	CHECK(std::strcmp(stats.row_average(sim_row), "0.67") == 0, "sim mean ms");
	CHECK(std::strstr(stats.row_info(sim_row), "0.7 t/f") != nullptr, "sim info from the SIM_TICKS value slot");
	CHECK(std::strstr(stats.row_info(frame_row), "fps") != nullptr, "frame info reports fps");
	CHECK(std::strlen(stats.row_average(find_row(stats, "frame_overhead"))) == 0, "header rows carry no time");

	tools.pass().set_open(false);
	CHECK(!board.is_capture_active(), "closing the tools disarms capture");
	tools.pass().set_open(true);
	tools.stats_window().open = false;
	ImGui::NewFrame();
	tools.pass().draw_frame(4);
	ImGui::Render();
	CHECK(!board.is_capture_active(), "closing the Stats window alone disarms capture");
	tools.set_frame_stats(nullptr);
}

void test_external_feed_drives_the_window_without_draining() {
	NullBackend backend;
	GameDevTools tools;
	FrameStatsBoard board;
	tools.pass().attach_imgui(backend.context, &test_alloc, &test_free, nullptr);
	tools.set_frame_stats(&board);
	tools.pass().set_open(true);
	CaptureWindow window;
	window.frames = 10;
	window.sums[static_cast<size_t>(Slot::FRAME_WALL)] = 100000;
	window.peaks[static_cast<size_t>(Slot::FRAME_WALL)] = 20000;
	window.sample_frames[static_cast<size_t>(Slot::FRAME_WALL)] = 10;
	tools.stats_window().feed_external(window);
	CHECK(!board.is_capture_active(), "an external feed releases the board");
	board.set_capture_active(true, 0);
	board.add(Slot::FRAME_WALL, 999999, 1);
	ImGui::NewFrame();
	tools.pass().draw_frame(2);
	ImGui::Render();
	const StatsWindow &stats = tools.stats_window();
	CHECK(std::strcmp(stats.row_average(find_row(stats, "frame")), "10.00") == 0, "the external reading is what renders");
	CHECK(board.drain(2).sums[static_cast<size_t>(Slot::FRAME_WALL)] == 999999, "the window did not drain the board");
	tools.set_frame_stats(nullptr);
}

}  // namespace

int main() {
	test_abi_fingerprint_is_the_pinned_one();
	test_attach_sets_docking_and_viewport_policy();
	test_layout_pass_draws_the_stats_window_and_gates_capture();
	test_external_feed_drives_the_window_without_draining();
	if (g_failures != 0) {
		std::printf("%d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("devtools_test: OK\n");
	return 0;
}
