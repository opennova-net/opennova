// The game's dev tools against a null ImGui backend: the context hand-off
// sets the docking/viewport policy, a layout pass with the Stats window open
// produces draw data, the Stats window arms and disarms the board's capture
// on its visibility edges and formats a drained window, and the ImGui ABI
// fingerprint is the pinned one (the imgui-godot addon rejects any other).
#include <runtime/devtools/debug_request.h>
#include <runtime/devtools/entities_window.h>
#include <runtime/devtools/environment_request.h>
#include <runtime/devtools/environment_snapshot.h>
#include <runtime/devtools/environment_window.h>
#include <runtime/devtools/game_dev_tools.h>
#include <runtime/devtools/game_window.h>
#include <runtime/devtools/imgui_abi.h>
#include <runtime/devtools/entity_directory_snapshot.h>
#include <runtime/devtools/stats_window.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdio>
#include <cstring>

using opennova::devtools::CaptureWindow;
using opennova::devtools::DebugRequest;
using opennova::devtools::EntitiesWindow;
using opennova::devtools::EnvironmentRequest;
using opennova::devtools::EnvironmentSnapshot;
using opennova::devtools::EnvironmentWindow;
using opennova::devtools::EntityDirectorySnapshot;
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

void test_game_window_is_mandatory_and_detachable() {
	GameDevTools tools;
	CHECK(tools.pass().window_count() == 5, "Game + Stats + Entities + Environment + demo registered");
	const opennova::devtools::Window &game = tools.pass().window(0);
	const opennova::devtools::Window &stats = tools.pass().window(1);
	CHECK(std::strcmp(game.title(), "Game") == 0, "Game is the first workspace window");
	CHECK(game.open, "Game opens from construction");
	CHECK(!game.is_closeable(), "Game is mandatory");
	CHECK(game.is_undockable(), "Game can detach from the application workspace");
	CHECK(game.initial_dock_placement() == InitialDockPlacement::Center,
			"Game owns the center dock");
	CHECK(stats.initial_dock_placement() == InitialDockPlacement::Right,
			"Stats starts in the right dock");
	CHECK(std::strcmp(tools.pass().window(2).title(), "Entities") == 0,
			"Entities registers after Stats");
	CHECK(!tools.pass().window(2).open, "the Entities window starts closed");
	CHECK(!tools.pass().window(3).open, "the demo window starts closed");
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
				(game->WindowClass.DockNodeFlagsOverrideSet & ImGuiDockNodeFlags_NoUndocking) == 0,
				"Game's dock node can be undocked");
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
	CHECK(tools.pass().window_count() == 5, "Game + Stats + Entities + Environment + demo registered");
	CHECK(tools.stats_window().open, "the Stats window opens by default");
	CHECK(!tools.pass().window(3).open, "the demo window starts closed");
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

// The Entities window (ADR 0042 d6): the pushed directory record formats into
// the filtered table, the layout pass draws it, and an invalid snapshot (the
// world unloaded) or a visibility close clears it.
void test_entities_window_formats_the_pushed_directory() {
	NullBackend backend;
	GameDevTools tools;
	tools.pass().attach_imgui(backend.context, &test_alloc, &test_free, nullptr);
	tools.pass().set_open(true);
	CHECK(!tools.needs_entity_directory(), "a closed Entities window needs no snapshot");
	tools.entities_window().open = true;
	CHECK(tools.needs_entity_directory(), "pass open && window open arms the feed");

	EntityDirectorySnapshot snapshot;
	opennova::world::inspect::EntityRow alpha;
	alpha.index = 0;
	alpha.ai_index = 2;
	alpha.editable = true;
	alpha.net_id = 1201;
	alpha.wire_handle = 0x3001;
	alpha.name = "ALPHA";
	alpha.health = 100;
	alpha.team = 1;
	alpha.alive = true;
	alpha.mission_position = {10.0f, 20.0f, 3.0f};
	snapshot.rows.push_back(alpha);
	opennova::world::inspect::EntityRow bravo;
	bravo.index = 1;
	bravo.ai_index = -1;
	bravo.net_id = 1202;
	bravo.wire_handle = 0x3002;
	bravo.name = "BRAVO";
	bravo.health = 0;
	bravo.team = 2;
	bravo.alive = false;
	snapshot.rows.push_back(bravo);
	snapshot.valid = true;
	snapshot.logic_tick = 62;
	tools.set_entity_directory(snapshot);

	ImGui::NewFrame();
	CHECK(tools.pass().draw_frame(1), "the workspace frame draws with the Entities window open");
	ImGui::Render();
	CHECK(ImGui::FindWindowByName("Entities") != nullptr, "the Entities window exists after a pass");

	const EntitiesWindow &entities = tools.entities_window();
	CHECK(entities.snapshot_valid(), "the pushed snapshot is the reading");
	CHECK(entities.snapshot_logic_tick() == 62, "the reading carries the join's logic tick");
	CHECK(entities.row_count() == 2, "one formatted row per pushed row");
	CHECK(std::strcmp(entities.row_name(0), "ALPHA") == 0, "name");
	CHECK(std::strcmp(entities.row_ai(0), "2") == 0, "ai index");
	CHECK(std::strcmp(entities.row_net_id(0), "1201") == 0, "ssn");
	CHECK(std::strcmp(entities.row_team(0), "1") == 0, "team");
	CHECK(std::strcmp(entities.row_health(0), "100") == 0, "health");
	CHECK(std::strcmp(entities.row_alive(0), "yes") == 0, "alive");
	CHECK(std::strcmp(entities.row_pos(0), "10.0 20.0 3.0") == 0, "mission position");
	CHECK(std::strcmp(entities.row_ai(1), "-") == 0, "a brainless row shows no ai index");
	CHECK(std::strcmp(entities.row_alive(1), "no") == 0, "a dead row reads no");

	tools.entities_window().set_filter("brav");
	CHECK(entities.row_count() == 1, "the filter narrows the table");
	CHECK(std::strcmp(entities.row_name(0), "BRAVO") == 0, "case-insensitive name match");
	tools.entities_window().set_filter("1201");
	CHECK(entities.row_count() == 1 && std::strcmp(entities.row_name(0), "ALPHA") == 0,
			"the filter also matches the SSN");
	tools.entities_window().set_filter("");
	CHECK(entities.row_count() == 2, "clearing the filter restores every row");

	tools.set_entity_directory(EntityDirectorySnapshot{});
	CHECK(!entities.snapshot_valid() && entities.row_count() == 0,
			"an invalid snapshot clears the table (the world unloaded)");

	tools.set_entity_directory(snapshot);
	CHECK(entities.row_count() == 2, "a re-push restores the table");
	tools.pass().set_open(false);
	CHECK(!tools.needs_entity_directory(), "closing the pass drops the need");
	CHECK(!entities.snapshot_valid() && entities.row_count() == 0,
			"the visibility close drops the held snapshot (a closed window costs nothing)");
}

// The DebugRequest channel: enqueue/take round-trips the typed payloads in
// order and drains exactly once; needs_entity_directory gates on (pass open
// && window open) so the embedder can skip building snapshots nobody shows.
void test_entities_debug_request_queue_and_gating() {
	GameDevTools tools;
	DebugRequest request;
	CHECK(!tools.take_debug_request(request), "fresh tools hold no debug request");
	CHECK(!tools.needs_entity_directory(), "closed pass: no directory needed");
	tools.entities_window().open = true;
	CHECK(!tools.needs_entity_directory(), "window open inside a closed pass still needs none");
	tools.pass().set_open(true);
	CHECK(tools.needs_entity_directory(), "pass open && window open");
	tools.entities_window().open = false;
	CHECK(!tools.needs_entity_directory(), "closing the window drops the need");
	tools.entities_window().open = true;

	DebugRequest health;
	health.kind = DebugRequest::Kind::SetEntityHealth;
	health.target.packed = 0x3001;
	health.health = 25;
	tools.entities_window().enqueue_request(health);
	DebugRequest teleport;
	teleport.kind = DebugRequest::Kind::TeleportLocalPlayer;
	teleport.pos[0] = 100.0f;
	teleport.pos[1] = 200.0f;
	teleport.pos[2] = 5.0f;
	teleport.yaw = 90.0f;
	teleport.pitch = -10.0f;
	tools.entities_window().enqueue_request(teleport);
	CHECK(tools.take_debug_request(request) && request.kind == DebugRequest::Kind::SetEntityHealth &&
					request.target.packed == 0x3001 && request.health == 25,
			"the health request round-trips first");
	CHECK(tools.take_debug_request(request) && request.kind == DebugRequest::Kind::TeleportLocalPlayer &&
					request.pos[0] == 100.0f && request.pos[1] == 200.0f && request.pos[2] == 5.0f &&
					request.yaw == 90.0f && request.pitch == -10.0f,
			"the teleport request follows with its payload");
	CHECK(!tools.take_debug_request(request), "the queue drains exactly once");
}

// The Environment window formats the retail page rows [orig:
// Debug_DrawEnvironmentValues @ 0x4ef000] from the pushed record, seeds its
// control strip from the live values, and drops everything on the
// visibility close.
void test_environment_window_formats_the_pushed_record() {
	NullBackend backend;
	GameDevTools tools;
	tools.pass().attach_imgui(backend.context, &test_alloc, &test_free, nullptr);
	tools.pass().set_open(true);
	CHECK(!tools.needs_environment_snapshot(), "a closed Environment window needs no snapshot");
	tools.environment_window().open = true;
	CHECK(tools.needs_environment_snapshot(), "pass open && window open arms the feed");

	EnvironmentSnapshot snapshot;
	snapshot.valid = true;
	snapshot.logic_tick = 310;
	snapshot.env_name = "full_00";
	snapshot.trn_name = "Dvxi1";
	snapshot.blink_flags = 0x02u | 0x20u;
	snapshot.fog_type = 2;
	snapshot.fog_dist_metres = 640;
	snapshot.fog_target_metres = 200;
	snapshot.color_fade_seconds = 3;
	snapshot.sun_fade_pct = 0;
	snapshot.night = true;
	snapshot.fog_rgb = 0x102030u;
	snapshot.sun_rgb = 0xE6D9BFu;
	snapshot.outdoor_rgb = 0x405060u;
	snapshot.gain_rgb = 0x404040u;
	snapshot.iris_rgb = 0x404040u;
	snapshot.fov_degrees = 80;
	snapshot.sky_height_metres = 175;
	snapshot.sky_speed = 15;
	snapshot.rain_pct = 37;
	snapshot.rain_target_pct = 100;
	snapshot.overcast_pct = 50;
	snapshot.overcast_target_pct = 50;
	snapshot.complexity = 12;
	snapshot.minute_of_day = 750;
	snapshot.quake_ticks = 42;
	snapshot.precipitation_kind = 1;
	snapshot.wind_scale = 256;
	snapshot.authority = true;
	tools.set_environment_snapshot(snapshot);

	ImGui::NewFrame();
	CHECK(tools.pass().draw_frame(1), "the workspace frame draws with the Environment window open");
	ImGui::Render();
	CHECK(ImGui::FindWindowByName("Environment") != nullptr, "the Environment window exists after a pass");

	const EnvironmentWindow &window = tools.environment_window();
	CHECK(window.snapshot_valid(), "the pushed snapshot is the reading");
	CHECK(window.row_count() == EnvironmentWindow::kRowCount, "every retail row formats");
	CHECK(std::strcmp(window.row_text(0), "Env: full_00") == 0, "Env row");
	CHECK(std::strcmp(window.row_text(1), "Trn: Dvxi1") == 0, "Trn row");
	CHECK(std::strcmp(window.row_text(2), "Blink: V---O") == 0, "the blink bits print V/S/W/L/O");
	CHECK(std::strcmp(window.row_text(3), "Fogtype: 2") == 0, "Fogtype row");
	CHECK(std::strcmp(window.row_text(4), "Fogdist: 640m") == 0, "Fogdist row (the hi word, metres)");
	CHECK(std::strcmp(window.row_text(5), "ColorFade: 3 seconds") == 0, "ColorFade row");
	CHECK(std::strcmp(window.row_text(6), "SunFade: 0%") == 0, "SunFade row");
	CHECK(std::strcmp(window.row_text(7), "MoonLight: 1") == 0, "MoonLight row");
	CHECK(std::strcmp(window.row_text(8), "Fog: (16, 32, 48)") == 0, "the color rows print (r, g, b)");
	CHECK(std::strcmp(window.row_text(11), "FOV: 80 degrees") == 0, "FOV row");
	CHECK(std::strcmp(window.row_text(12), "Sun: (230, 217, 191)") == 0, "Sun row");
	CHECK(std::strcmp(window.row_text(18), "SkyHeight: 175m") == 0, "SkyHeight row");
	CHECK(std::strcmp(window.row_text(19), "SkySpeed: 15m") == 0, "SkySpeed row (the retail format)");
	CHECK(std::strcmp(window.row_text(20), "OutDoor: (64, 80, 96)") == 0, "OutDoor row");
	CHECK(std::strcmp(window.row_text(24), "Rain: 37%") == 0, "Rain row");
	CHECK(std::strcmp(window.row_text(25), "Overcast: 50%") == 0, "Overcast row");
	CHECK(std::strcmp(window.row_text(26), "Complexity: 12") == 0, "Complexity row");
	CHECK(std::strcmp(window.row_text(27), "DCB: 692") == 0, "the DCB literal");
	CHECK(window.rain_percent_edit() == 100, "the control strip seeds from the live rain target");

	tools.set_environment_snapshot(EnvironmentSnapshot{});
	CHECK(!window.snapshot_valid() && window.row_count() == 0,
			"an invalid snapshot clears the page (the world unloaded)");
	tools.set_environment_snapshot(snapshot);
	CHECK(window.row_count() == EnvironmentWindow::kRowCount, "a re-push restores the page");
	tools.pass().set_open(false);
	CHECK(!tools.needs_environment_snapshot(), "closing the pass drops the need");
	CHECK(!window.snapshot_valid(), "the visibility close drops the held snapshot");
}

// The EnvironmentRequest channel: enqueue/take round-trips the typed weather
// commands in order and drains exactly once.
void test_environment_request_queue() {
	GameDevTools tools;
	EnvironmentRequest request;
	CHECK(!tools.take_environment_request(request), "fresh tools hold no environment request");
	tools.environment_window().enqueue_request({EnvironmentRequest::Kind::Rain, 100, 5});
	tools.environment_window().enqueue_request({EnvironmentRequest::Kind::MoveFog, 200, 2});
	tools.environment_window().enqueue_request({EnvironmentRequest::Kind::Flash, 0, 0});
	CHECK(tools.take_environment_request(request) && request.kind == EnvironmentRequest::Kind::Rain &&
					request.a == 100 && request.b == 5,
			"the rain request round-trips first");
	CHECK(tools.take_environment_request(request) && request.kind == EnvironmentRequest::Kind::MoveFog &&
					request.a == 200 && request.b == 2,
			"the move-fog request follows");
	CHECK(tools.take_environment_request(request) && request.kind == EnvironmentRequest::Kind::Flash,
			"the flash request follows");
	CHECK(!tools.take_environment_request(request), "the queue drains exactly once");
}

}  // namespace

int main() {
	test_abi_fingerprint_is_the_pinned_one();
	test_attach_sets_docking_and_viewport_policy();
	test_game_window_is_mandatory_and_detachable();
	test_game_window_sends_responsive_integer_content_size_to_its_adapter();
	test_game_window_orders_play_interact_and_close_requests();
	test_default_workspace_layout_is_created_once_and_preserves_user_layout();
	test_layout_pass_draws_the_stats_window_and_gates_capture();
	test_external_feed_drives_the_window_without_draining();
	test_layout_reset_brings_windows_home();
	test_entities_window_formats_the_pushed_directory();
	test_entities_debug_request_queue_and_gating();
	test_environment_window_formats_the_pushed_record();
	test_environment_request_queue();
	if (g_failures != 0) {
		std::printf("%d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("devtools_test: OK\n");
	return 0;
}
