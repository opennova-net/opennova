// The game's dev tools against a null ImGui backend: the context hand-off
// sets the docking/viewport policy, a layout pass with the Stats window open
// produces draw data, the Stats window arms and disarms the board's capture
// on its visibility edges and formats a drained window, and the ImGui ABI
// fingerprint is the pinned one (the imgui-godot addon rejects any other).
#include <runtime/devtools/game_dev_tools.h>
#include <runtime/devtools/game_window.h>
#include <runtime/devtools/imgui_abi.h>
#include <runtime/devtools/stats_window.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdio>
#include <cstring>

using opennova::devtools::CaptureWindow;
using opennova::devtools::GameViewport;
using opennova::devtools::GameWindow;
using opennova::devtools::GameInputMode;
using opennova::devtools::GameWindowRequest;
using opennova::devtools::InitialDockPlacement;
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

struct FakeGameViewport : GameViewport {
	int width = 0;
	int height = 0;
	int draws = 0;

	void draw(int requested_width, int requested_height) override {
		width = requested_width;
		height = requested_height;
		++draws;
	}
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
	CHECK(tools.pass().platform_windows_enabled(), "multi-viewport is the attach default");
	tools.pass().set_platform_windows_enabled(false);
	CHECK((io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) == 0,
			"the shell suspends multi-viewport for a fullscreen main window");
	CHECK((io.ConfigFlags & ImGuiConfigFlags_DockingEnable) != 0, "docking survives the suspension");
	tools.pass().set_platform_windows_enabled(true);
	CHECK((io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0, "and restores it once windowed again");
	CHECK(tools.pass().attach_imgui(backend.context, &test_alloc, &test_free, nullptr), "re-attach is harmless (reloadable extension)");
	tools.pass().detach_imgui();
	CHECK(!tools.pass().is_attached(), "detached");
	CHECK(ImGui::GetCurrentContext() == nullptr, "detach drops the engine copy's context binding");
	ImGui::SetCurrentContext(backend.context);
}

void test_game_window_is_the_mandatory_center_surface() {
	GameDevTools tools;
	CHECK(tools.pass().window_count() == 3, "Game + Stats + demo registered");
	const opennova::devtools::Window &game = tools.pass().window(0);
	const opennova::devtools::Window &stats = tools.pass().window(1);
	CHECK(std::strcmp(game.title(), "Game") == 0, "Game is the first workspace window");
	CHECK(game.open, "Game opens from construction");
	CHECK(!game.is_closeable(), "Game is mandatory");
	CHECK(!game.is_undockable(), "Game stays in the application workspace");
	CHECK(game.initial_dock_placement() == InitialDockPlacement::Center,
			"Game owns the center dock");
	CHECK(stats.initial_dock_placement() == InitialDockPlacement::Right,
			"Stats starts in the right dock");
	CHECK(!tools.pass().window(2).open, "the demo window starts closed");
}

void test_game_window_sends_responsive_integer_content_size_to_its_adapter() {
	NullBackend backend;
	opennova::devtools::ImGuiPass pass;
	pass.attach_imgui(backend.context, &test_alloc, &test_free, nullptr);
	GameWindow game;
	FakeGameViewport viewport;
	game.set_viewport(&viewport);

	auto draw_at = [&](float width, float height, uint64_t frame) {
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
		ImGui::Begin("Game viewport harness", nullptr,
				ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
		game.draw(pass, frame);
		ImGui::End();
		ImGui::Render();
	};

	draw_at(640.0f, 480.0f, 1);
	const int first_width = viewport.width;
	const int first_height = viewport.height;
	CHECK(viewport.draws == 1, "the viewport adapter draws once per visible Game frame");
	CHECK(first_width > 0 && first_height > 0, "the adapter receives a usable content size");
	draw_at(800.0f, 600.0f, 2);
	CHECK(viewport.draws == 2, "the next visible frame draws once again");
	CHECK(viewport.width - first_width == 160, "content width follows the window pixel delta");
	CHECK(viewport.height - first_height == 120, "content height follows the window pixel delta");

	game.set_viewport(nullptr);
	draw_at(800.0f, 600.0f, 3);
	CHECK(viewport.draws == 2, "the null adapter makes engine-only/headless drawing inert");
}

void test_game_window_orders_play_interact_and_close_requests() {
	GameWindow game;
	GameWindowRequest request = GameWindowRequest::CloseTools;
	CHECK(game.input_mode() == GameInputMode::Interact, "Game starts in Interact");
	CHECK(!game.play_available(), "Play starts unavailable until the shell enables it");
	game.request_enter_play();
	CHECK(!game.take_request(request), "unavailable Play cannot queue a request");

	game.set_play_available(true);
	game.request_enter_play();
	game.set_input_mode(GameInputMode::Play);
	game.request_escape();
	game.set_input_mode(GameInputMode::Interact);
	game.request_escape();
	CHECK(game.take_request(request) && request == GameWindowRequest::EnterPlay,
			"Play is the first request");
	CHECK(game.take_request(request) && request == GameWindowRequest::EnterInteract,
			"Escape from Play returns to Interact");
	CHECK(game.take_request(request) && request == GameWindowRequest::CloseTools,
			"Escape from Interact closes the tools");
	CHECK(!game.take_request(request), "the request queue drains exactly once");

	game.set_input_mode(GameInputMode::Play);
	game.set_play_available(false);
	CHECK(game.take_request(request) && request == GameWindowRequest::EnterInteract,
			"losing Play availability forces Interact");
}

void test_default_workspace_layout_is_created_once_and_preserves_user_layout() {
	NullBackend backend;
	auto draw = [](GameDevTools &tools, uint64_t frame) {
		ImGui::NewFrame();
		CHECK(tools.pass().draw_frame(frame), "the workspace frame draws");
		ImGui::Render();
	};

	ImGuiID center_id = 0;
	{
		GameDevTools tools;
		tools.pass().attach_imgui(backend.context, &test_alloc, &test_free, nullptr);
		tools.pass().set_open(true);
		draw(tools, 1);
		ImGuiWindow *game = ImGui::FindWindowByName("Game");
		ImGuiWindow *stats = ImGui::FindWindowByName("Stats");
		CHECK(game != nullptr && stats != nullptr, "default Game and Stats windows exist");
		CHECK(game != nullptr && game->DockId != 0, "Game is docked on first use");
		CHECK(stats != nullptr && stats->DockId != 0, "Stats is docked on first use");
		CHECK(game != nullptr && stats != nullptr && game->DockId != stats->DockId,
				"Game and Stats start in separate center/right docks");
		CHECK(game != nullptr && stats != nullptr && stats->Pos.x > game->Pos.x,
				"Stats occupies the roughly 30% right-hand dock");
		CHECK(game != nullptr &&
				(game->Flags & (ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
						ImGuiWindowFlags_NoScrollWithMouse)) ==
						(ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
								ImGuiWindowFlags_NoScrollWithMouse),
				"Game has no collapse control or scrollbars");
		CHECK(game != nullptr &&
				(game->WindowClass.DockNodeFlagsOverrideSet & ImGuiDockNodeFlags_NoUndocking) != 0,
				"Game's dock node cannot be undocked");
		ImGuiDockNode *root = game != nullptr && game->DockNode != nullptr
				? ImGui::DockNodeGetRootNode(game->DockNode)
				: nullptr;
		CHECK(root != nullptr && (root->MergedFlags & ImGuiDockNodeFlags_PassthruCentralNode) == 0,
				"the application workspace is opaque");

		center_id = game != nullptr ? game->DockId : 0;
		if (center_id != 0) {
			ImGui::DockBuilderDockWindow("Stats", center_id);
		}
		draw(tools, 2);
		stats = ImGui::FindWindowByName("Stats");
		CHECK(stats != nullptr && stats->DockId == center_id,
				"a user can move Stats into the center dock");
	}

	ImGui::SetCurrentContext(backend.context);
	GameDevTools restored;
	restored.pass().attach_imgui(backend.context, &test_alloc, &test_free, nullptr);
	restored.pass().set_open(true);
	draw(restored, 3);
	ImGuiWindow *restored_stats = ImGui::FindWindowByName("Stats");
	CHECK(restored_stats != nullptr && restored_stats->DockId == center_id,
			"an existing layout is not overwritten by default placement");
	restored.pass().window(0).open = false;
	draw(restored, 4);
	CHECK(restored.pass().window(0).open, "the mandatory Game window cannot be closed");
}

void test_layout_pass_draws_the_stats_window_and_gates_capture() {
	NullBackend backend;
	GameDevTools tools;
	FrameStatsBoard board;
	tools.pass().attach_imgui(backend.context, &test_alloc, &test_free, nullptr);
	tools.set_frame_stats(&board);
	CHECK(tools.pass().window_count() == 3, "Game + Stats + demo registered");
	CHECK(tools.stats_window().open, "the Stats window opens by default");
	CHECK(!tools.pass().window(2).open, "the demo window starts closed");
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
	// The feed lasts one visibility session: closing and reopening the tools
	// re-arms the board and the window drains it again.
	tools.pass().set_open(false);
	tools.pass().set_open(true);
	CHECK(board.is_capture_active(), "reopening after an external feed re-arms capture");
	// The reopened window starts at the last drawn frame (2); two frames of
	// 30 ms wall time drain at frame 4.
	board.add(Slot::FRAME_WALL, 30000, 3);
	board.add(Slot::FRAME_WALL, 30000, 4);
	ImGui::NewFrame();
	tools.pass().draw_frame(4);
	ImGui::Render();
	CHECK(stats.reading_frames() == 2, "the reading spans the frames since the reopen");
	CHECK(std::strcmp(stats.row_average(find_row(stats, "frame")), "30.00") == 0, "the window drains the board again");
	tools.set_frame_stats(nullptr);
}

// A parked Stats window (collapsed, pushed off to where a second monitor
// would be) comes home on the layout pass after a reset request: expanded,
// undocked, cascaded from the main viewport's work corner.
void test_layout_reset_brings_windows_home() {
	NullBackend backend;
	GameDevTools tools;
	tools.pass().attach_imgui(backend.context, &test_alloc, &test_free, nullptr);
	tools.pass().set_open(true);
	CHECK(!tools.pass().is_layout_reset_pending(), "nothing pending on fresh tools");
	ImGui::NewFrame();
	tools.pass().draw_frame(1);
	ImGui::Render();
	ImGuiWindow *stats = ImGui::FindWindowByName("Stats");
	CHECK(stats != nullptr, "the Stats window exists after a pass");
	if (stats == nullptr) {
		return;
	}
	// The default workspace layout docks Stats into the right node beside the
	// mandatory Game center; a docked window ignores a parked position, so the
	// "away from home" state here IS the docked one.
	ImGui::NewFrame();
	tools.pass().draw_frame(2);
	ImGui::Render();
	CHECK(stats->DockId != 0, "parked in the default right dock node");

	tools.pass().request_layout_reset();
	CHECK(tools.pass().is_layout_reset_pending(), "a reset is pending");
	ImGui::NewFrame();
	tools.pass().draw_frame(3);
	ImGui::Render();
	CHECK(!tools.pass().is_layout_reset_pending(), "the pass consumed the reset");
	const ImGuiViewport *main = ImGui::GetMainViewport();
	// The cascade steps 32 px per registration index: Game is 0, Stats is 1.
	const float home_x = main->WorkPos.x + 24.0f + 32.0f;
	const float home_y = main->WorkPos.y + 24.0f + 32.0f;
	CHECK(!stats->Collapsed, "expanded");
	CHECK(stats->DockId == 0, "undocked");
	CHECK(stats->Pos.x == home_x && stats->Pos.y == home_y,
			"home = the work corner cascade at the window's registration index");
	CHECK(stats->Viewport == ImGui::GetMainViewport(), "inside the main viewport");
	ImGui::NewFrame();
	tools.pass().draw_frame(4);
	ImGui::Render();
	CHECK(stats->Pos.x == home_x, "a one-shot: the next pass leaves placement alone");
}

}  // namespace

int main() {
	test_abi_fingerprint_is_the_pinned_one();
	test_attach_sets_docking_and_viewport_policy();
	test_game_window_is_the_mandatory_center_surface();
	test_game_window_sends_responsive_integer_content_size_to_its_adapter();
	test_game_window_orders_play_interact_and_close_requests();
	test_default_workspace_layout_is_created_once_and_preserves_user_layout();
	test_layout_pass_draws_the_stats_window_and_gates_capture();
	test_external_feed_drives_the_window_without_draining();
	test_layout_reset_brings_windows_home();
	if (g_failures != 0) {
		std::printf("%d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("devtools_test: OK\n");
	return 0;
}
