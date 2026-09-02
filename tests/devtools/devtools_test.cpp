// The game's dev tools against a null ImGui backend: the context hand-off
// sets the docking/viewport policy, a layout pass with the Stats window open
// produces draw data, the Stats window arms and disarms the board's capture
// on its visibility edges and formats a drained window, and the ImGui ABI
// fingerprint is the pinned one (the imgui-godot addon rejects any other).
#include <runtime/devtools/ai_debug_snapshot.h>
#include <runtime/devtools/ai_window.h>
#include <runtime/devtools/debug_request.h>
#include <runtime/devtools/entities_window.h>
#include <runtime/devtools/environment_request.h>
#include <runtime/devtools/environment_snapshot.h>
#include <runtime/devtools/environment_window.h>
#include <runtime/devtools/rays_window.h>
#include <runtime/devtools/game_dev_tools.h>
#include <runtime/devtools/game_window.h>
#include <runtime/devtools/imgui_abi.h>
#include <runtime/devtools/entity_detail_snapshot.h>
#include <runtime/devtools/entity_directory_snapshot.h>
#include <runtime/devtools/entity_properties_window.h>
#include <runtime/devtools/stats_window.h>
#include <formats/def/def.h>
#include <runtime/devtools/physics_request.h>
#include <runtime/devtools/physics_window.h>
#include <runtime/devtools/weapon_request.h>
#include <runtime/devtools/weapon_window.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdio>
#include <cstring>
#include <utility>

using opennova::devtools::AiDebugSnapshot;
using opennova::devtools::AiWindow;
using opennova::devtools::CaptureWindow;
using opennova::devtools::DebugRequest;
using opennova::devtools::EntitiesWindow;
using opennova::devtools::EntityDetailSnapshot;
using opennova::devtools::EnvironmentRequest;
using opennova::devtools::EnvironmentSnapshot;
using opennova::devtools::EnvironmentWindow;
using opennova::devtools::PhysicsRequest;
using opennova::devtools::PhysicsSnapshot;
using opennova::devtools::PhysicsWindow;
using opennova::devtools::RaysRequest;
using opennova::devtools::RaysSnapshot;
using opennova::devtools::RaysWindow;
using opennova::devtools::EntityDirectorySnapshot;
using opennova::devtools::EntityPropertiesWindow;
using opennova::devtools::GameViewport;
using opennova::devtools::GameWindow;
using opennova::devtools::GameInputMode;
using opennova::devtools::GameWindowRequest;
using opennova::devtools::InitialDockPlacement;
using opennova::devtools::GameDevTools;
using opennova::devtools::FrameStatsBoard;
using opennova::devtools::Slot;
using opennova::devtools::StatsWindow;
using opennova::devtools::WeaponWindow;

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
	CHECK(tools.pass().window_count() == 10,
			"Game + Stats + Entities + Entity Properties + Weapon + Environment + AI + Rays + Physics + demo registered");
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
	CHECK(std::strcmp(tools.pass().window(3).title(), "Entity Properties") == 0,
			"Entity Properties registers after Entities (it reads that window's selection)");
	CHECK(!tools.pass().window(3).open, "the Entity Properties window starts closed");
	CHECK(tools.pass().window(3).initial_dock_placement() == InitialDockPlacement::RightBottom,
			"Entity Properties starts under the right column, its own dock node");
	CHECK(std::strcmp(tools.pass().window(4).title(), "Weapon") == 0,
			"Weapon registers after Entity Properties");
	CHECK(!tools.pass().window(4).open, "the Weapon window starts closed");
	CHECK(tools.pass().window(4).initial_dock_placement() == InitialDockPlacement::None,
			"Weapon floats: the right dock is too narrow for a timeline");
	CHECK(tools.pass().window(4).preferred_size().width > 0.0f,
			"and asks for a first-open size wide enough to draw one");
	CHECK(std::strcmp(tools.pass().window(5).title(), "Environment") == 0,
			"Environment registers after Weapon");
	CHECK(!tools.pass().window(5).open, "the Environment window starts closed");
	CHECK(std::strcmp(tools.pass().window(6).title(), "AI") == 0,
			"AI registers after Environment (it reads the Entities selection)");
	CHECK(!tools.pass().window(6).open, "the AI window starts closed");
	CHECK(tools.pass().window(6).initial_dock_placement() == InitialDockPlacement::RightBottom,
			"AI starts under the right column beside the card");
	CHECK(std::strcmp(tools.pass().window(7).title(), "Rays") == 0,
			"Rays registers after AI");
	CHECK(!tools.pass().window(7).open, "the Rays window starts closed");
	CHECK(std::strcmp(tools.pass().window(8).title(), "Physics") == 0,
			"Physics registers after Rays");
	CHECK(!tools.pass().window(8).open, "the Physics window starts closed");
	CHECK(!tools.pass().window(9).open, "the demo window starts closed");
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

	CHECK(!game.spectator_available() && !game.spectator_active(),
			"spectator mutation starts unavailable and off");
	game.request_spectator(true);
	CHECK(!game.take_request(request), "an unavailable spectator toggle queues nothing");
	game.set_spectator_state(true, false);
	game.request_spectator(true);
	CHECK(game.take_request(request) && request == GameWindowRequest::EnableSpectator,
			"the F3 control requests the authoritative spectator transition");
	game.set_spectator_state(true, true);
	game.request_spectator(false);
	CHECK(game.take_request(request) && request == GameWindowRequest::DisableSpectator,
			"the F3 control requests returning to a player");
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
	CHECK(tools.pass().window_count() == 10,
			"Game + Stats + Entities + Entity Properties + Weapon + Environment + AI + Rays + Physics + demo registered");
	CHECK(tools.stats_window().open, "the Stats window opens by default");
	CHECK(!tools.pass().window(9).open, "the demo window starts closed");
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

// A reset request rebuilds the default docked layout on the next layout
// pass: a window the persisted layout never placed (opened later, dragged
// out, or new since the ini was written) docks into its declared node,
// collapsed windows expand, and the pass consumes the request once.
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
	ImGuiWindow *game = ImGui::FindWindowByName("Game");
	CHECK(stats != nullptr && game != nullptr, "the Game and Stats windows exist after a pass");
	if (stats == nullptr || game == nullptr) {
		return;
	}
	ImGui::NewFrame();
	tools.pass().draw_frame(2);
	ImGui::Render();
	CHECK(stats->DockId != 0, "Stats docked in the default right dock node");

	// The entity windows open after the layout was created (a pick does
	// this): the persisted dockspace has no place for them, so they float,
	// and Stats is pulled out of its node the way a user drag would.
	tools.entities_window().open = true;
	tools.entity_properties_window().open = true;
	ImGui::SetWindowDock(stats, 0, ImGuiCond_Always);
	stats->Collapsed = true;
	ImGui::NewFrame();
	tools.pass().draw_frame(3);
	ImGui::Render();
	ImGuiWindow *entities = ImGui::FindWindowByName("Entities");
	ImGuiWindow *properties = ImGui::FindWindowByName("Entity Properties");
	CHECK(entities != nullptr && properties != nullptr, "the entity windows exist once open");
	if (entities == nullptr || properties == nullptr) {
		return;
	}
	ImGui::NewFrame();
	tools.pass().draw_frame(4);
	ImGui::Render();
	CHECK(stats->DockId == 0, "Stats undocked (the away-from-home state)");

	tools.pass().request_layout_reset();
	CHECK(tools.pass().is_layout_reset_pending(), "a reset is pending");
	ImGui::NewFrame();
	tools.pass().draw_frame(5);
	ImGui::Render();
	CHECK(!tools.pass().is_layout_reset_pending(), "the pass consumed the reset");
	ImGui::NewFrame();
	tools.pass().draw_frame(6);
	ImGui::Render();
	CHECK(!stats->Collapsed, "expanded");
	CHECK(stats->DockId != 0, "Stats is docked again");
	CHECK(entities->DockId == stats->DockId, "Entities shares the right node with Stats");
	CHECK(properties->DockId != 0 && properties->DockId != stats->DockId,
			"Entity Properties docks into its own node under the right column");
	CHECK(game->DockId != 0 && game->DockId != stats->DockId, "Game keeps the center node");
	CHECK(properties->Pos.y > stats->Pos.y, "the Properties node sits below the right node");
	CHECK(stats->Pos.x > game->Pos.x, "the right column sits beside the Game center");
	CHECK(stats->Viewport == ImGui::GetMainViewport(), "inside the main viewport");
	const ImGuiID stats_node = stats->DockId;
	ImGui::SetWindowDock(stats, 0, ImGuiCond_Always);
	ImGui::NewFrame();
	tools.pass().draw_frame(7);
	ImGui::Render();
	ImGui::NewFrame();
	tools.pass().draw_frame(8);
	ImGui::Render();
	CHECK(stats->DockId != stats_node, "a one-shot: a later user undock is left alone");
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

namespace {

EntityDirectorySnapshot two_row_directory() {
	EntityDirectorySnapshot snapshot;
	opennova::world::inspect::EntityRow alpha;
	alpha.index = 0;
	alpha.ai_index = 2;
	alpha.editable = true;
	alpha.net_id = 1201;
	alpha.wire_handle = 0x3001;
	alpha.name = "ALPHA";
	alpha.item_name = "Rifleman";
	alpha.health = 100;
	alpha.team = 1;
	alpha.alive = true;
	snapshot.rows.push_back(alpha);
	opennova::world::inspect::EntityRow bravo;
	bravo.index = 1;
	bravo.ai_index = -1;
	bravo.net_id = 1202;
	bravo.wire_handle = 0x1002;
	bravo.name = "BRAVO";
	bravo.item_name = "Humvee";
	bravo.team = 2;
	snapshot.rows.push_back(bravo);
	snapshot.valid = true;
	snapshot.logic_tick = 62;
	return snapshot;
}

EntityDetailSnapshot detail_for(uint16_t handle, uint32_t attrib, uint32_t attrib2) {
	EntityDetailSnapshot detail;
	detail.card.valid = true;
	detail.card.handle = handle;
	detail.card.has_world = true;
	detail.card.world.handle = handle;
	detail.card.world.item_attrib = attrib;
	detail.card.world.item_attrib2 = attrib2;
	detail.card.world.health_max = 120;
	detail.card.world.item_name = "Rifleman";
	detail.logic_tick = 70;
	return detail;
}

}  // namespace

// The selection seam (the shell's world pick): selecting a handle opens and
// focuses the window, selects the row when the pushed directory has it and
// otherwise waits for the push that carries it; the selection survives the
// visibility close as a pending handle, and a push without it drops it.
void test_entities_window_selects_the_picked_handle() {
	NullBackend backend;
	GameDevTools tools;
	tools.pass().attach_imgui(backend.context, &test_alloc, &test_free, nullptr);
	tools.pass().set_open(true);
	EntitiesWindow &entities = tools.entities_window();
	CHECK(tools.selected_entity_handle() == opennova::world::EntityHandle::kInvalid,
			"nothing selected at first");
	CHECK(!tools.needs_entity_detail(), "no selection: no detail needed");

	// A pick before any push: both windows open and ask for focus, the list
	// holds the handle pending.
	tools.select_entity(0x3001);
	CHECK(entities.open, "a pick opens the Entities window");
	CHECK(entities.focus_requested(), "a pick asks the pass to focus the window");
	CHECK(tools.entity_properties_window().open, "a pick opens the Entity Properties window too");
	CHECK(tools.entity_properties_window().focus_requested(), "...and focuses it in its own dock node");
	CHECK(tools.selected_entity_handle() == 0x3001, "the pending handle reads as the selection");
	CHECK(tools.needs_entity_detail(), "a pending selection already wants its detail card");
	CHECK(!entities.wants_scroll_to_selected(), "no row to scroll to yet");

	tools.entities_window().set_filter("brav");
	tools.set_entity_directory(two_row_directory());
	CHECK(entities.row_count() == 2, "the pick cleared the filter so the row can show");
	CHECK(tools.selected_entity_handle() == 0x3001, "the push applied the pending selection");
	CHECK(entities.wants_scroll_to_selected(), "the applied selection scrolls into view");
	CHECK(std::strcmp(entities.row_item(0), "Rifleman") == 0, "the Item column names the def");
	CHECK(std::strcmp(entities.row_item(1), "Humvee") == 0, "the Item column names the def (2)");
	tools.entities_window().set_filter("humv");
	CHECK(entities.row_count() == 1 && std::strcmp(entities.row_name(0), "BRAVO") == 0,
			"the filter matches the item name too");
	tools.entities_window().set_filter("");

	ImGui::NewFrame();
	CHECK(tools.pass().draw_frame(1), "the workspace frame draws with a selection");
	ImGui::Render();
	CHECK(ImGui::FindWindowByName("Entity Properties") != nullptr,
			"the Entity Properties window exists after a pass");
	CHECK(entities.focus_requested(),
			"a window opening this frame keeps its focus request for the frame after (its tab exists then)");
	CHECK(!entities.wants_scroll_to_selected(), "the drawn table consumed the scroll request");
	ImGui::NewFrame();
	CHECK(tools.pass().draw_frame(2), "the next workspace frame draws");
	ImGui::Render();
	CHECK(!entities.focus_requested(), "the second layout pass consumed the focus request");
	CHECK(!tools.entity_properties_window().focus_requested(), "...both of them");

	// A pick of a row the directory already holds selects it at once.
	tools.select_entity(0x1002);
	CHECK(tools.selected_entity_handle() == 0x1002, "a held row selects immediately");
	CHECK(entities.wants_scroll_to_selected(), "...and scrolls into view");

	// The visibility close keeps the selection pending; the next push re-selects.
	tools.pass().set_open(false);
	CHECK(!entities.snapshot_valid() && entities.row_count() == 0, "closing drops the snapshot");
	CHECK(tools.selected_entity_handle() == 0x1002, "the selection survives the close as pending");
	CHECK(!tools.needs_entity_detail(), "a closed pass wants no detail card");
	tools.pass().set_open(true);
	tools.set_entity_directory(two_row_directory());
	CHECK(tools.selected_entity_handle() == 0x1002, "reopening and pushing re-selects the entity");

	// The selection follows its handle across a reorder: the other row moves
	// into the selected index and must not inherit the selection.
	EntityDirectorySnapshot reordered = two_row_directory();
	std::swap(reordered.rows[0], reordered.rows[1]);
	tools.set_entity_directory(reordered);
	CHECK(tools.selected_entity_handle() == 0x1002, "a reorder keeps the selected entity");
	CHECK(std::strcmp(entities.row_name(0), "BRAVO") == 0, "...while the rows moved");

	// A push without the selected entity drops the selection (it is gone);
	// removing the FIRST row is the case where the old index now names the
	// other entity.
	tools.select_entity(0x3001);
	EntityDirectorySnapshot without = two_row_directory();
	without.rows.erase(without.rows.begin());
	tools.set_entity_directory(without);
	CHECK(tools.selected_entity_handle() == opennova::world::EntityHandle::kInvalid,
			"a push without the entity clears the selection instead of sliding to the next row");
	tools.set_entity_directory(two_row_directory());

	tools.select_entity(0x3001);
	tools.clear_entity_selection();
	CHECK(tools.selected_entity_handle() == opennova::world::EntityHandle::kInvalid,
			"clear_entity_selection empties it");
	CHECK(!tools.needs_entity_detail(), "...and nothing is wanted");
	tools.select_entity(0x3001);
	tools.entity_properties_window().open = false;
	CHECK(!tools.needs_entity_detail(), "a closed Properties window wants no card");
	tools.entity_properties_window().open = true;
	CHECK(tools.needs_entity_detail(), "reopening it wants the card again");

	// Closing the list while the card stays open keeps the directory flowing
	// (the card reads the list's row) and the selection re-applies per push.
	tools.entities_window().open = false;
	ImGui::NewFrame();
	tools.pass().draw_frame(3);
	ImGui::Render();
	CHECK(tools.needs_entity_directory(), "an open Properties window keeps the directory pushes");
	CHECK(tools.selected_entity_handle() == 0x3001, "the closed list holds the selection pending");
	tools.set_entity_directory(two_row_directory());
	CHECK(entities.selected_row() != nullptr && entities.selected_row()->wire_handle == 0x3001,
			"the next push resolves the row for the card even with the list closed");
	tools.entities_window().open = true;

	// A pick the directory never carries keeps the user's filter.
	tools.entities_window().set_filter("brav");
	tools.select_entity(0x0777);
	CHECK(std::strcmp(entities.filter(), "brav") == 0, "an unresolved pick leaves the filter");
	tools.set_entity_directory(two_row_directory());
	CHECK(std::strcmp(entities.filter(), "brav") == 0 &&
					tools.selected_entity_handle() == opennova::world::EntityHandle::kInvalid,
			"a push without it drops the pick and still leaves the filter");
	tools.entities_window().set_filter("");

	// A closed window carries no focus request forward: the pick opened it,
	// the user closed it again before its next Begin.
	tools.select_entity(0x3001);
	tools.entities_window().open = false;
	tools.entity_properties_window().open = false;
	ImGui::NewFrame();
	tools.pass().draw_frame(4);
	ImGui::Render();
	CHECK(!entities.focus_requested() && !tools.entity_properties_window().focus_requested(),
			"closing the windows drops their focus requests");
	tools.entities_window().open = true;
	tools.entity_properties_window().open = true;
	tools.set_entity_directory(two_row_directory());

	// The invalid handle is a clear, never a selection (pool 0 slot 0 = 0 is valid).
	tools.select_entity(0);
	CHECK(tools.selected_entity_handle() == 0, "handle 0 is a real entity (pool 0, slot 0)");
	tools.select_entity(opennova::world::EntityHandle::kInvalid);
	CHECK(tools.selected_entity_handle() == opennova::world::EntityHandle::kInvalid,
			"selecting kInvalid clears");
}

// The Entity Properties window: a pushed card for the selected handle
// populates the pane and seeds the edit words; a card for another handle is
// dropped; a toggle flips the edit word and leaves as one SetEntityItemAttrib
// request carrying both full words through the Entities window's queue;
// selection changes and the visibility close clear the card.
void test_entity_properties_window_card_and_attrib_toggles() {
	NullBackend backend;
	GameDevTools tools;
	tools.pass().attach_imgui(backend.context, &test_alloc, &test_free, nullptr);
	tools.pass().set_open(true);
	EntityPropertiesWindow &properties = tools.entity_properties_window();
	tools.entities_window().open = true;
	tools.entity_properties_window().open = true;
	EntityDirectorySnapshot directory = two_row_directory();
	directory.authority = true;
	tools.set_entity_directory(directory);

	tools.set_entity_detail(detail_for(0x3001, DEF_ITEM_ATTRIB_NODISMEMBER, 0));
	CHECK(!properties.detail_valid(), "a card for an unselected handle is dropped");

	tools.select_entity(0x3001);
	tools.set_entity_detail(detail_for(0x1002, DEF_ITEM_ATTRIB_NODIE, 0));
	CHECK(!properties.detail_valid(), "a card for another handle is dropped");
	tools.set_entity_detail(detail_for(0x3001,
			DEF_ITEM_ATTRIB_NODISMEMBER | DEF_ITEM_ATTRIB_AIDATA, DEF_ITEM_ATTRIB2_FARP));
	CHECK(properties.detail_valid() && properties.detail_handle() == 0x3001, "the selected card lands");
	CHECK(properties.detail_attrib() == (DEF_ITEM_ATTRIB_NODISMEMBER | DEF_ITEM_ATTRIB_AIDATA),
			"the attrib edit word seeds from the card");
	CHECK(properties.detail_attrib2() == DEF_ITEM_ATTRIB2_FARP, "the attrib2 edit word seeds too");
	CHECK(properties.detail_health_max() == 120, "health_max reads from the card");
	CHECK(std::strcmp(properties.detail_item_name(), "Rifleman") == 0, "the item name reads from the card");

	ImGui::NewFrame();
	CHECK(tools.pass().draw_frame(1), "the workspace frame draws with a detail card");
	ImGui::Render();

	DebugRequest request;
	CHECK(!tools.take_debug_request(request), "no request before a toggle");
	properties.toggle_item_attrib(DEF_ITEM_ATTRIB_NODISMEMBER);
	CHECK(properties.detail_attrib() == DEF_ITEM_ATTRIB_AIDATA, "the toggle clears the bit locally");
	CHECK(tools.take_debug_request(request) &&
					request.kind == DebugRequest::Kind::SetEntityItemAttrib &&
					request.target.packed == 0x3001 && request.attrib == DEF_ITEM_ATTRIB_AIDATA &&
					request.attrib2 == DEF_ITEM_ATTRIB2_FARP,
			"one request carries both full words behind the selected handle");
	properties.toggle_item_attrib2(DEF_ITEM_ATTRIB2_LANDMINE);
	CHECK(tools.take_debug_request(request) &&
					request.attrib == DEF_ITEM_ATTRIB_AIDATA &&
					request.attrib2 == (DEF_ITEM_ATTRIB2_FARP | DEF_ITEM_ATTRIB2_LANDMINE),
			"the second word toggles the same way");
	CHECK(!tools.take_debug_request(request), "the queue drains exactly once");

	// A re-push re-seeds the edit words (the engine truth wins).
	tools.set_entity_detail(detail_for(0x3001, DEF_ITEM_ATTRIB_NODIE, 0));
	CHECK(properties.detail_attrib() == DEF_ITEM_ATTRIB_NODIE && properties.detail_attrib2() == 0,
			"a re-push re-seeds the edit words");

	// Selecting another row invalidates the card until its own push lands.
	tools.select_entity(0x1002);
	CHECK(!properties.detail_valid(), "another selection never shows the previous card");
	tools.set_entity_detail(detail_for(0x1002, 0, 0));
	CHECK(properties.detail_valid(), "its own card lands");
	properties.toggle_item_attrib(DEF_ITEM_ATTRIB_NOSCAR);
	CHECK(tools.take_debug_request(request) && request.target.packed == 0x1002 &&
					request.attrib == DEF_ITEM_ATTRIB_NOSCAR,
			"a brainless row (a vehicle) takes attrib overrides too");

	// An invalid card clears; so does the Properties window's visibility close
	// (the Entities window keeps the selection pending meanwhile).
	tools.set_entity_detail(EntityDetailSnapshot{});
	CHECK(!properties.detail_valid(), "an invalid card clears the pane");
	tools.set_entity_detail(detail_for(0x1002, 0, 0));
	tools.pass().set_open(false);
	CHECK(!properties.detail_valid(), "the visibility close drops the card");
	CHECK(tools.selected_entity_handle() == 0x1002, "the selection survives as pending");
	properties.toggle_item_attrib(DEF_ITEM_ATTRIB_NOSCAR);
	CHECK(!tools.take_debug_request(request), "a toggle without a card is a no-op");
}

// The authority gate: the pushed snapshot says whether this peer owns the
// world; without it the Properties window queues nothing (its controls draw
// disabled), and while a wire session is live the AIData bit stays refused.
void test_entity_edits_gate_on_the_pushed_authority() {
	NullBackend backend;
	GameDevTools tools;
	tools.pass().attach_imgui(backend.context, &test_alloc, &test_free, nullptr);
	tools.pass().set_open(true);
	EntityPropertiesWindow &properties = tools.entity_properties_window();
	EntitiesWindow &entities = tools.entities_window();
	entities.open = true;
	properties.open = true;
	CHECK(!entities.authority(), "no push: no authority");

	// A joiner's push: everything reads, nothing writes.
	EntityDirectorySnapshot joiner = two_row_directory();
	joiner.authority = false;
	tools.set_entity_directory(joiner);
	tools.select_entity(0x3001);
	tools.set_entity_detail(detail_for(0x3001, DEF_ITEM_ATTRIB_NODISMEMBER, 0));
	CHECK(properties.detail_valid(), "the joiner still gets the card");
	CHECK(!properties.edits_enabled(), "...but no edits");
	ImGui::NewFrame();
	CHECK(tools.pass().draw_frame(1), "the workspace draws read-only");
	ImGui::Render();
	DebugRequest request;
	properties.toggle_item_attrib(DEF_ITEM_ATTRIB_NODISMEMBER);
	properties.toggle_item_attrib2(DEF_ITEM_ATTRIB2_FARP);
	CHECK(!tools.take_debug_request(request), "a toggle without authority queues nothing");
	CHECK(properties.detail_attrib() == DEF_ITEM_ATTRIB_NODISMEMBER, "...and moves no bit");

	// The authority's push with a live wire session: edits work, AIData stays.
	EntityDirectorySnapshot live = two_row_directory();
	live.authority = true;
	live.session_live = true;
	tools.set_entity_directory(live);
	CHECK(entities.authority() && entities.session_live(), "the facts ride the snapshot");
	CHECK(properties.edits_enabled(), "authority enables the edits");
	properties.toggle_item_attrib(DEF_ITEM_ATTRIB_AIDATA);
	CHECK(!tools.take_debug_request(request), "the AIData bit is locked while a session is live");
	properties.toggle_item_attrib(DEF_ITEM_ATTRIB_NODISMEMBER);
	CHECK(tools.take_debug_request(request) && request.attrib == 0,
			"the other bits toggle under a live session");

	// Single player: AIData toggles too.
	EntityDirectorySnapshot single = two_row_directory();
	single.authority = true;
	tools.set_entity_directory(single);
	properties.toggle_item_attrib(DEF_ITEM_ATTRIB_AIDATA);
	CHECK(tools.take_debug_request(request) && request.attrib == DEF_ITEM_ATTRIB_AIDATA,
			"AIData toggles without a wire session");

	// An invalid push (the world unloaded) takes the authority with it.
	tools.set_entity_directory(EntityDirectorySnapshot{});
	CHECK(!entities.authority() && !properties.edits_enabled(), "no world: no authority, no edits");
}

// A definition shaped like the shipped minimal weapon.def: FIRE authored
// {0, 5}, RELOAD authored {196, auto} baked to 31 from its clip, RECOIL {0, 0}
// (the zero-length tail that a frame-rate sampler would miss), and SCOPEUP
// left unauthored the way every shipped corpus leaves it.
opennova::devtools::WeaponDefinitionSnapshot make_weapon_definition() {
	namespace wa = opennova::world::weapon_action;
	opennova::devtools::WeaponDefinitionSnapshot def;
	def.valid = true;
	def.serial = 1;
	def.weapon_name = "WPN_AK47AUTO";
	def.adm_index = 2;
	def.clip_capacity = 30;
	def.auto_fire = true;
	for (int id = 0; id < wa::kCount; ++id) {
		opennova::devtools::WeaponActionRow &row = def.actions[id];
		row.authored = id <= wa::kSwitchRank;  // the nine rows shipped data writes
		row.authored_name = "IDLE";
		row.function = "WPN_STD_IDLE";
	}
	opennova::devtools::WeaponActionRow &fire = def.actions[wa::kFire];
	fire.authored_name = "FIRE";
	fire.function = "WPN_STD_FIRE";
	fire.delay_start = 0;
	fire.delay_end = 5;
	fire.authored_delay_start = 0;
	fire.authored_delay_end = 5;
	fire.has_anim = true;
	fire.anim_key = "anim_wpn_fire";
	fire.soundsetend = "GS_M4";  // the gunshot rides the END leg on most rows
	fire.clip_ticks = 7;

	opennova::devtools::WeaponActionRow &recoil = def.actions[wa::kRecoil];
	recoil.authored_name = "RECOIL";
	recoil.function = "WPN_STD_RECOIL";
	recoil.particle = "Effect_CAR15MF";

	opennova::devtools::WeaponActionRow &reload = def.actions[wa::kReload];
	reload.authored_name = "RELOAD";
	reload.function = "WPN_STD_RELOAD";
	reload.delay_start = 196;
	reload.delay_end = 31;
	reload.authored_delay_start = 196;
	reload.authored_delay_end = -1;  // `auto`
	reload.has_anim = true;
	reload.anim_key = "anim_wpn_reload";
	reload.clip_ticks = 31;
	return def;
}

opennova::devtools::WeaponLiveSnapshot make_weapon_live(uint64_t tick = 8800) {
	opennova::devtools::WeaponLiveSnapshot live;
	live.valid = true;
	live.logic_tick = tick;
	live.clip = 29;
	live.reserve = 210;
	live.current = opennova::world::weapon_action::kRecoil;
	live.phase = opennova::world::weapon_phase::kActive;
	live.counter = 3;
	live.trace_armed = true;
	return live;
}

opennova::world::WeaponTraceSample make_trace_sample(uint32_t tick, int32_t current,
		uint8_t phase, int32_t counter) {
	opennova::world::WeaponTraceSample sample;
	sample.tick = tick;
	sample.current = current;
	sample.phase = phase;
	sample.counter = counter;
	return sample;
}

void drain_all(GameDevTools &tools) {
	opennova::devtools::WeaponRequest request;
	while (tools.take_weapon_request(request)) {
	}
}

void test_weapon_window_formats_the_pushed_definition() {
	namespace wa = opennova::world::weapon_action;
	GameDevTools tools;
	WeaponWindow &weapon = tools.weapon_window();
	CHECK(!weapon.definition_valid(), "a fresh window holds nothing");

	weapon.set_definition(make_weapon_definition());
	CHECK(weapon.definition_valid(), "the pushed definition lands");
	CHECK(std::strcmp(weapon.weapon_name(), "WPN_AK47AUTO") == 0, "the equipped name");
	// An explicit delay reads as itself; `auto` reads as auto AND what it baked
	// to, because those are different authorings the editor must not conflate.
	CHECK(std::strcmp(weapon.action_timing(wa::kFire), "0 / 5") == 0,
			"explicit delays read as their values");
	CHECK(std::strcmp(weapon.action_timing(wa::kReload), "196 / auto(31)") == 0,
			"an auto delay shows the sentinel and its baked value");
	CHECK(std::strcmp(weapon.action_timing(wa::kRecoil), "0 / 0") == 0,
			"a zero-length tail is still a value, not a blank");
	CHECK(std::strcmp(weapon.action_label(wa::kFire), "FIRE") == 0, "authored rows read plainly");
	CHECK(std::strcmp(weapon.action_label(wa::kScopeUp), "SCOPEUP *") == 0,
			"a slot with no authored row is marked: its edits cannot be written back");

	// An invalid definition clears the window (the world unloaded).
	weapon.set_definition(opennova::devtools::WeaponDefinitionSnapshot{});
	CHECK(!weapon.definition_valid(), "an invalid definition clears");
	CHECK(weapon.trace_count() == 0, "and drops the trace with it");
}

void test_weapon_window_accumulates_the_trace_delta() {
	namespace wa = opennova::world::weapon_action;
	namespace wp = opennova::world::weapon_phase;
	GameDevTools tools;
	WeaponWindow &weapon = tools.weapon_window();
	weapon.set_definition(make_weapon_definition());

	// The embedder pushes only what the pump recorded since the last push; the
	// window is what accumulates them into a scrollback.
	opennova::devtools::WeaponLiveSnapshot first = make_weapon_live();
	first.trace.push_back(make_trace_sample(8801, wa::kFire, wp::kEntered, 0));
	opennova::world::WeaponTraceSample fired = make_trace_sample(8802, wa::kRecoil, wp::kActive, 4);
	fired.fired = true;
	fired.action_finished = wa::kFire;
	first.trace.push_back(fired);
	weapon.set_live(std::move(first));
	CHECK(weapon.trace_count() == 2, "the first delta lands");

	opennova::devtools::WeaponLiveSnapshot second = make_weapon_live();
	second.trace.push_back(make_trace_sample(8803, wa::kRecoil, wp::kActive, 3));
	weapon.set_live(std::move(second));
	CHECK(weapon.trace_count() == 3, "the next delta appends rather than replacing");
	CHECK(std::strcmp(weapon.trace_row(0), "8801 FIRE ENTER c0") == 0,
			"a plain tick formats as tick/action/phase/counter");
	CHECK(std::strcmp(weapon.trace_row(1), "8802 RECOIL ACTIVE c4 fired end=FIRE") == 0,
			"and an eventful tick names what happened on it");

	// A re-push of ticks already held (the embedder re-primed) appends only
	// the new ones.
	opennova::devtools::WeaponLiveSnapshot again = make_weapon_live();
	again.trace.push_back(make_trace_sample(8803, wa::kRecoil, wp::kActive, 3));
	again.trace.push_back(make_trace_sample(8804, wa::kRecoil, wp::kActive, 2));
	weapon.set_live(std::move(again));
	CHECK(weapon.trace_count() == 4, "the duplicate tick is skipped, the new one lands");

	// A restarted logic clock (a round restart) restarts the scrollback rather
	// than being dropped as stale or drawn across a boundary that never was.
	opennova::devtools::WeaponLiveSnapshot restarted = make_weapon_live(40);
	restarted.trace.push_back(make_trace_sample(40, wa::kIdle, wp::kDone, 5));
	weapon.set_live(std::move(restarted));
	CHECK(weapon.trace_count() == 1, "a rewound clock starts the scrollback over");

	// A different weapon is a different definition: nothing carries over.
	opennova::devtools::WeaponDefinitionSnapshot other = make_weapon_definition();
	other.weapon_name = "WPN_M4AUTO";
	other.serial = 2;
	weapon.set_definition(std::move(other));
	CHECK(weapon.trace_count() == 0, "switching weapons drops the previous trace");

	// A small delta rides the playhead; a reopen that pulls more than a
	// second of recording asks for View All so the whole burst is on screen.
	CHECK(!weapon.trace_fit_pending(), "no fit pending on a fresh scrollback");
	opennova::devtools::WeaponLiveSnapshot burst = make_weapon_live(200);
	for (uint32_t t = 100; t < 200; ++t) {
		burst.trace.push_back(make_trace_sample(t, wa::kFire, wp::kActive, 1));
	}
	weapon.set_live(std::move(burst));
	CHECK(weapon.trace_count() == 100, "the reopen batch lands");
	CHECK(weapon.trace_fit_pending(), "and the trace will View All on the next layout");
}

void test_weapon_window_request_queue_and_gating() {
	namespace wa = opennova::world::weapon_action;
	using opennova::devtools::WeaponRequest;
	GameDevTools tools;
	WeaponWindow &weapon = tools.weapon_window();

	CHECK(!weapon.wants_records(), "a closed window asks the embedder for nothing");
	CHECK(!tools.needs_weapon_records(), "and the gate agrees");
	weapon.open = true;
	CHECK(!tools.needs_weapon_records(), "a window open inside a closed pass still wants nothing");
	tools.pass().set_open(true);

	WeaponRequest request;
	// Becoming visible arms the engine ring; that IS the first request.
	CHECK(tools.take_weapon_request(request), "visibility queues the trace arm");
	CHECK(request.kind == WeaponRequest::Kind::ArmTrace && request.armed, "armed on the way in");
	CHECK(weapon.is_recording(), "and the window says it is recording");
	CHECK(weapon.wants_records(), "a visible window wants records");
	CHECK(tools.needs_weapon_records(), "and the embedder gate opens");

	// The drawn controls all funnel through enqueue_request, which is also the
	// headless seam: clicking a real button needs a real backend.
	WeaponRequest trigger;
	trigger.kind = WeaponRequest::Kind::TriggerAction;
	trigger.trigger = WeaponRequest::Trigger::Reload;
	weapon.enqueue_request(trigger);
	CHECK(tools.take_weapon_request(request), "the queue drains through GameDevTools");
	CHECK(request.kind == WeaponRequest::Kind::TriggerAction &&
					request.trigger == WeaponRequest::Trigger::Reload,
			"and it carries the real input seam, not a raw action id");
	CHECK(!tools.take_weapon_request(request), "the queue is empty");

	// An arm queued with no world behind it drains and drops; the live record
	// then reports the ring disarmed and the window asks again until the
	// engine agrees.
	weapon.set_definition(make_weapon_definition());
	opennova::devtools::WeaponLiveSnapshot disarmed = make_weapon_live();
	disarmed.trace_armed = false;
	weapon.set_live(std::move(disarmed));
	CHECK(tools.take_weapon_request(request) && request.kind == WeaponRequest::Kind::ArmTrace &&
					request.armed,
			"a disarmed ring under REC re-queues the arm");
	weapon.set_live(make_weapon_live());
	CHECK(!tools.take_weapon_request(request), "an armed ring under REC asks for nothing");

	// Hiding drops the live record but not the definition or the scrollback,
	// always releases the hold, and with REC on leaves the engine ring armed:
	// the loop is close F3, shoot, reopen, read.
	opennova::devtools::WeaponLiveSnapshot held = make_weapon_live();
	held.trace.push_back(make_trace_sample(9000, wa::kIdle, opennova::world::weapon_phase::kDone, 3));
	weapon.set_live(std::move(held));
	tools.pass().set_open(false);
	CHECK(!weapon.wants_records(), "hidden again");
	CHECK(!weapon.live_valid(), "it drops the live record it held");
	CHECK(weapon.definition_valid(), "keeps the definition (the identity for the reopen)");
	CHECK(weapon.trace_count() == 1, "and keeps the scrollback");
	CHECK(tools.take_weapon_request(request) && request.kind == WeaponRequest::Kind::SetFireHeld &&
					!request.held,
			"a hold cannot outlive the window: hiding releases it");
	CHECK(!tools.take_weapon_request(request), "and queues no disarm while REC is on");

	// The reopen of the SAME weapon keeps the scrollback; a different weapon
	// would not (identity survives the hide).
	tools.pass().set_open(true);
	drain_all(tools);
	weapon.set_definition(make_weapon_definition());
	CHECK(weapon.trace_count() == 1, "the same weapon on reopen keeps the scrollback");

	// REC off releases the ring on the way out.
	weapon.set_recording_for_test(false);
	drain_all(tools);
	tools.pass().set_open(false);
	bool saw_release = false;
	bool saw_disarm = false;
	while (tools.take_weapon_request(request)) {
		if (request.kind == WeaponRequest::Kind::SetFireHeld && !request.held) saw_release = true;
		if (request.kind == WeaponRequest::Kind::ArmTrace && !request.armed) saw_disarm = true;
	}
	CHECK(saw_release, "the hold is released on every hide");
	CHECK(saw_disarm, "hiding with REC off queues the disarm");
}

void test_weapon_window_delay_edits_keep_the_other_legs_authoring() {
	namespace wa = opennova::world::weapon_action;
	using opennova::devtools::WeaponRequest;
	GameDevTools tools;
	WeaponWindow &weapon = tools.weapon_window();
	weapon.set_definition(make_weapon_definition());
	WeaponRequest request;

	// RELOAD is authored {196, auto}. Retiming its start must not freeze its
	// end: the untouched leg travels as authored (-1), and no re-bake is asked
	// for because nothing new needs resolving.
	weapon.request_delay_edit(wa::kReload, WeaponWindow::DelayLeg::Start, 197);
	CHECK(tools.take_weapon_request(request) && request.kind == WeaponRequest::Kind::SetActionDelays,
			"a start edit queues one delay request");
	CHECK(request.action_id == wa::kReload && request.delay_start == 197 && request.delay_end == -1 &&
					!request.rebake,
			"the start is explicit, the end stays `auto`, no re-bake");

	weapon.request_delay_edit(wa::kReload, WeaponWindow::DelayLeg::End, 40);
	CHECK(tools.take_weapon_request(request) && request.delay_start == 196 && request.delay_end == 40 &&
					!request.rebake,
			"an end edit freezes the end and carries the authored start");

	// Turning a leg to `auto` is the one edit that needs the clip: rebake.
	weapon.request_delay_auto(wa::kFire, WeaponWindow::DelayLeg::End, true);
	CHECK(tools.take_weapon_request(request) && request.action_id == wa::kFire &&
					request.delay_start == 0 && request.delay_end == -1 && request.rebake,
			"auto on: the leg goes -1 and asks for the re-bake");
	// Turning it off freezes what the clip baked, with no re-bake.
	weapon.request_delay_auto(wa::kReload, WeaponWindow::DelayLeg::End, false);
	CHECK(tools.take_weapon_request(request) && request.delay_start == 196 && request.delay_end == 31 &&
					!request.rebake,
			"auto off: the baked value is frozen, no re-bake");

	// A slot with no authored row can only carry its baked values.
	weapon.request_delay_auto(wa::kScopeUp, WeaponWindow::DelayLeg::End, true);
	CHECK(!tools.take_weapon_request(request), "an unauthored slot has no row to write `auto` into");
	weapon.request_delay_edit(wa::kScopeUp, WeaponWindow::DelayLeg::End, 3);
	CHECK(tools.take_weapon_request(request) && request.delay_start == 0 && request.delay_end == 3,
			"but its live delays still edit");
}

void test_weapon_window_draws_its_panes() {
	NullBackend backend;
	GameDevTools tools;
	tools.pass().attach_imgui(backend.context, &test_alloc, &test_free, nullptr);
	WeaponWindow &weapon = tools.weapon_window();
	weapon.open = true;
	tools.pass().set_open(true);

	// The dope sheet and the trace are custom draw-list geometry rather than
	// stock widgets, so the layout pass is the only thing that exercises them.
	weapon.set_definition(make_weapon_definition());
	opennova::devtools::WeaponLiveSnapshot live = make_weapon_live();
	live.trace.push_back(make_trace_sample(8801, opennova::world::weapon_action::kFire,
			opennova::world::weapon_phase::kEntered, 0));
	live.trace.push_back(make_trace_sample(8802, opennova::world::weapon_action::kRecoil,
			opennova::world::weapon_phase::kActive, 4));
	weapon.set_live(std::move(live));

	ImGui::NewFrame();
	CHECK(tools.pass().draw_frame(11), "the pass drew");
	ImGui::Render();
	CHECK(ImGui::GetDrawData()->TotalVtxCount > 0, "the Weapon panes produced geometry");

	// And with no weapon it degrades to a sentence rather than dividing by a
	// zero-length axis.
	weapon.set_definition(opennova::devtools::WeaponDefinitionSnapshot{});
	ImGui::NewFrame();
	CHECK(tools.pass().draw_frame(12), "an empty window still lays out");
	ImGui::Render();
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
	tools.environment_window().enqueue_request({EnvironmentRequest::Kind::BlockColor, 2, 0x102030});
	CHECK(tools.take_environment_request(request) && request.kind == EnvironmentRequest::Kind::Rain &&
					request.a == 100 && request.b == 5,
			"the rain request round-trips first");
	CHECK(tools.take_environment_request(request) && request.kind == EnvironmentRequest::Kind::MoveFog &&
					request.a == 200 && request.b == 2,
			"the move-fog request follows");
	CHECK(tools.take_environment_request(request) && request.kind == EnvironmentRequest::Kind::Flash,
			"the flash request follows");
	CHECK(tools.take_environment_request(request) &&
					request.kind == EnvironmentRequest::Kind::BlockColor && request.a == 2 &&
					request.b == 0x102030,
			"the block color request carries its target and packed rgb");
	CHECK(!tools.take_environment_request(request), "the queue drains exactly once");
}

// The Rays window formats per-category count rows from the pushed record,
// mirrors the filter state into its edit controls, and drops everything on
// the visibility close.
void test_rays_window_formats_the_pushed_record() {
	NullBackend backend;
	GameDevTools tools;
	tools.pass().attach_imgui(backend.context, &test_alloc, &test_free, nullptr);
	tools.pass().set_open(true);
	CHECK(!tools.needs_rays_snapshot(), "a closed Rays window needs no snapshot");
	tools.rays_window().open = true;
	CHECK(tools.needs_rays_snapshot(), "pass open && window open arms the feed");

	RaysSnapshot snapshot;
	snapshot.valid = true;
	snapshot.logic_tick = 620;
	snapshot.recording = true;
	snapshot.category_mask = 0x7FFF;
	snapshot.ttl_ticks = 93;
	snapshot.categories[0].name = "Uncategorized";
	snapshot.categories[1].name = "Projectile";
	snapshot.categories[1].held = 12;
	snapshot.categories[1].total = 340;
	for (int i = 2; i < opennova::devtools::kRayCategoryCount; ++i) {
		snapshot.categories[i].name = "x";
	}
	tools.set_rays_snapshot(snapshot);

	ImGui::NewFrame();
	CHECK(tools.pass().draw_frame(1), "the workspace frame draws with the Rays window open");
	ImGui::Render();
	CHECK(ImGui::FindWindowByName("Rays") != nullptr, "the Rays window exists after a pass");

	const RaysWindow &window = tools.rays_window();
	CHECK(window.snapshot_valid(), "the pushed snapshot is the reading");
	CHECK(window.row_count() == opennova::devtools::kRayCategoryCount,
			"every category formats a row");
	CHECK(std::strcmp(window.row_text(0), "Uncategorized: held 0 / total 0") == 0,
			"an idle category row");
	CHECK(std::strcmp(window.row_text(1), "Projectile: held 12 / total 340") == 0,
			"held rides the ring, total is the lifetime counter");

	tools.set_rays_snapshot(RaysSnapshot{});
	CHECK(!window.snapshot_valid() && window.row_count() == 0,
			"an invalid snapshot clears the page (the world unloaded)");
	tools.set_rays_snapshot(snapshot);
	CHECK(window.row_count() == opennova::devtools::kRayCategoryCount,
			"a re-push restores the page");
	tools.pass().set_open(false);
	CHECK(!tools.needs_rays_snapshot(), "closing the pass drops the need");
	CHECK(!window.snapshot_valid(), "the visibility close drops the held snapshot");
}

// The RaysRequest channel: enqueue/take round-trips the typed filter, TTL
// and clear requests in order and drains exactly once.
void test_rays_request_queue() {
	GameDevTools tools;
	RaysRequest request;
	CHECK(!tools.take_rays_request(request), "fresh tools hold no rays request");
	tools.rays_window().enqueue_request({RaysRequest::Kind::SetCategoryMask, 0x0003});
	tools.rays_window().enqueue_request({RaysRequest::Kind::SetTtlTicks, 310});
	tools.rays_window().enqueue_request({RaysRequest::Kind::Clear, 0});
	CHECK(tools.take_rays_request(request) &&
					request.kind == RaysRequest::Kind::SetCategoryMask && request.a == 0x0003,
			"the mask request round-trips first");
	CHECK(tools.take_rays_request(request) &&
					request.kind == RaysRequest::Kind::SetTtlTicks && request.a == 310,
			"the TTL request follows");
	CHECK(tools.take_rays_request(request) && request.kind == RaysRequest::Kind::Clear,
			"the clear request follows");
	CHECK(!tools.take_rays_request(request), "the queue drains exactly once");
}

// The Physics window formats per-kind contact rows from the pushed record,
// mirrors the capture state into its edit controls, and drops everything on
// the visibility close.
void test_physics_window_formats_the_pushed_record() {
	NullBackend backend;
	GameDevTools tools;
	tools.pass().attach_imgui(backend.context, &test_alloc, &test_free, nullptr);
	tools.pass().set_open(true);
	CHECK(!tools.needs_physics_snapshot(), "a closed Physics window needs no snapshot");
	tools.physics_window().open = true;
	CHECK(tools.needs_physics_snapshot(), "pass open && window open arms the feed");

	PhysicsSnapshot snapshot;
	snapshot.valid = true;
	snapshot.logic_tick = 620;
	snapshot.capturing = true;
	snapshot.kind_mask = 0x3F;
	snapshot.recent = 3;
	snapshot.kinds[0].name = "Projectile hit";
	snapshot.kinds[0].held = 12;
	snapshot.kinds[0].total = 340;
	snapshot.kinds[1].name = "Knife hit";
	for (int i = 2; i < opennova::devtools::kContactKindCount; ++i) {
		snapshot.kinds[i].name = "x";
	}
	tools.set_physics_snapshot(snapshot);

	ImGui::NewFrame();
	CHECK(tools.pass().draw_frame(1), "the workspace frame draws with the Physics window open");
	ImGui::Render();
	CHECK(ImGui::FindWindowByName("Physics") != nullptr, "the Physics window exists after a pass");

	const PhysicsWindow &window = tools.physics_window();
	CHECK(window.snapshot_valid(), "the pushed snapshot is the reading");
	CHECK(window.row_count() == opennova::devtools::kContactKindCount,
			"every contact kind formats a row");
	CHECK(std::strcmp(window.row_text(0), "Projectile hit: held 12 / total 340") == 0,
			"held rides the ring, total is the lifetime counter");
	CHECK(std::strcmp(window.row_text(1), "Knife hit: held 0 / total 0") == 0,
			"an idle kind row");

	tools.set_physics_snapshot(PhysicsSnapshot{});
	CHECK(!window.snapshot_valid() && window.row_count() == 0,
			"an invalid snapshot clears the page (the world unloaded)");
	tools.set_physics_snapshot(snapshot);
	CHECK(window.row_count() == opennova::devtools::kContactKindCount,
			"a re-push restores the page");
	tools.pass().set_open(false);
	CHECK(!tools.needs_physics_snapshot(), "closing the pass drops the need");
	CHECK(!window.snapshot_valid(), "the visibility close drops the held snapshot");
}

// The PhysicsRequest channel: enqueue/take round-trips the typed mask, clear
// and capture requests in order and drains exactly once.
void test_physics_request_queue() {
	GameDevTools tools;
	PhysicsRequest request;
	CHECK(!tools.take_physics_request(request), "fresh tools hold no physics request");
	tools.physics_window().enqueue_request({PhysicsRequest::Kind::SetKindMask, 0x0005});
	tools.physics_window().enqueue_request({PhysicsRequest::Kind::Clear, 0});
	tools.physics_window().enqueue_request({PhysicsRequest::Kind::SetCaptureEnabled, 1});
	CHECK(tools.take_physics_request(request) &&
					request.kind == PhysicsRequest::Kind::SetKindMask && request.a == 0x0005,
			"the mask request round-trips first");
	CHECK(tools.take_physics_request(request) && request.kind == PhysicsRequest::Kind::Clear,
			"the clear request follows");
	CHECK(tools.take_physics_request(request) &&
					request.kind == PhysicsRequest::Kind::SetCaptureEnabled && request.a == 1,
			"the capture arm carries its state");
	CHECK(!tools.take_physics_request(request), "the queue drains exactly once");
}

}  // namespace

AiDebugSnapshot ai_snapshot() {
	AiDebugSnapshot snapshot;
	snapshot.valid = true;
	snapshot.logic_tick = 62;
	opennova::world::inspect::AiGroupRow group;
	group.id = 5;
	group.alert = 2;
	group.initial_count = 4;
	group.live_count = 3;
	snapshot.report.groups.push_back(group);
	opennova::world::inspect::AiNavChannelRow channel;
	channel.index = 1;
	channel.loopflag = 1;
	channel.nodes.resize(3);
	channel.followers = 2;
	snapshot.report.channels.push_back(channel);
	snapshot.report.counters.brain_count = 7;
	snapshot.report.counters.scheduler_budget = 128;
	snapshot.report.counters.unported_calls = 2;
	return snapshot;
}

EntityDetailSnapshot ai_detail_for(uint16_t handle) {
	EntityDetailSnapshot detail;
	detail.card.valid = true;
	detail.card.handle = handle;
	detail.card.ai_index = 3;
	detail.card.has_ai = true;
	detail.card.ai.name = "ALPHA";
	detail.card.ai.state = 17;
	detail.card.ai.state_name = "GROUND_COMBAT";
	detail.card.ai.alert = 2;
	detail.card.ai.target_valid = true;
	detail.card.ai.target_handle = 0x1002;
	detail.card.ai.target_name = "BRAVO";
	detail.card.ai.fire_delay = 9;
	detail.card.ai.infantry = true;
	detail.card.ai.aim_valid = true;
	detail.card.ai.profile_type = 3;
	detail.card.ai.slot_control_bits = 0x200;
	detail.logic_tick = 70;
	return detail;
}

// The AI window: the pushed snapshot formats the counters/groups/routes rows,
// an invalid push clears, needs_ai_debug gates on (pass open && window open),
// and the visibility close drops the records.
void test_ai_window_formats_the_pushed_snapshot() {
	NullBackend backend;
	GameDevTools tools;
	tools.pass().attach_imgui(backend.context, &test_alloc, &test_free, nullptr);
	AiWindow &ai = tools.ai_window();

	CHECK(!tools.needs_ai_debug(), "a closed pass wants no AI snapshot");
	tools.pass().set_open(true);
	CHECK(!tools.needs_ai_debug(), "a closed AI window wants no snapshot");
	ai.open = true;
	CHECK(tools.needs_ai_debug(), "pass open && window open wants the snapshot");

	CHECK(!ai.snapshot_valid(), "no snapshot before a push");
	tools.set_ai_debug(ai_snapshot());
	CHECK(ai.snapshot_valid(), "the push lands");
	CHECK(std::strstr(ai.counters_text(), "brains 7") != nullptr &&
					std::strstr(ai.counters_text(), "budget 128") != nullptr &&
					std::strstr(ai.counters_text(), "unported 2") != nullptr,
			"the counters line carries the system readings");
	CHECK(ai.group_count() == 1, "one group row");
	CHECK(std::strstr(ai.group_text(0), "G05") != nullptr &&
					std::strstr(ai.group_text(0), "RED") != nullptr &&
					std::strstr(ai.group_text(0), "3/4") != nullptr,
			"the group row names id, alert and live/initial counts");
	CHECK(ai.channel_count() == 1, "one channel row");
	CHECK(std::strstr(ai.channel_text(0), "ch 1") != nullptr &&
					std::strstr(ai.channel_text(0), "once") != nullptr &&
					std::strstr(ai.channel_text(0), "nodes 3") != nullptr &&
					std::strstr(ai.channel_text(0), "followers 2") != nullptr,
			"the channel row names index, loop mode, nodes and followers");

	ImGui::NewFrame();
	CHECK(tools.pass().draw_frame(1), "the workspace frame draws with the AI window open");
	ImGui::Render();

	tools.set_ai_debug(AiDebugSnapshot{});
	CHECK(!ai.snapshot_valid(), "an invalid push clears (the world unloaded)");
	CHECK(ai.group_count() == 0 && ai.channel_count() == 0, "...and the rows with it");
	CHECK(std::strcmp(ai.counters_text(), "No world.") == 0, "the counters line says so");

	tools.set_ai_debug(ai_snapshot());
	tools.pass().set_open(false);
	CHECK(!ai.snapshot_valid(), "the visibility close drops the snapshot");
	CHECK(!tools.needs_ai_debug(), "...and the pushes stop on the same edge");
}

// The deep pane rides the Entities selection and the same detail push the
// Entity Properties window receives; the widened needs_entity_detail keeps
// the card flowing for the AI window alone.
void test_ai_window_detail_pane_follows_the_selection() {
	NullBackend backend;
	GameDevTools tools;
	tools.pass().attach_imgui(backend.context, &test_alloc, &test_free, nullptr);
	tools.pass().set_open(true);
	AiWindow &ai = tools.ai_window();
	ai.open = true;
	tools.set_entity_directory(two_row_directory());

	tools.set_entity_detail(ai_detail_for(0x3001));
	CHECK(!ai.detail_valid(), "a card for an unselected handle is dropped");

	tools.select_entity(0x3001);
	tools.set_entity_detail(ai_detail_for(0x1002));
	CHECK(!ai.detail_valid(), "a card for another handle is dropped");
	tools.set_entity_detail(ai_detail_for(0x3001));
	CHECK(ai.detail_valid(), "the selected card lands");
	CHECK(ai.detail_line_count() > 0, "the pane formats lines");
	bool saw_state = false;
	bool saw_target = false;
	bool saw_berserk = false;
	for (int i = 0; i < ai.detail_line_count(); ++i) {
		if (std::strstr(ai.detail_line(i), "GROUND_COMBAT") != nullptr) saw_state = true;
		if (std::strstr(ai.detail_line(i), "BRAVO") != nullptr) saw_target = true;
		if (std::strstr(ai.detail_line(i), "BERSERK") != nullptr) saw_berserk = true;
	}
	CHECK(saw_state, "the pane names the state");
	CHECK(saw_target, "the pane names the target");
	CHECK(saw_berserk, "the pane decodes the control bits");

	ImGui::NewFrame();
	CHECK(tools.pass().draw_frame(1), "the workspace frame draws with the pane");
	ImGui::Render();

	// The widened gate: with the Properties window closed (the pick opened
	// it), the AI window alone keeps the detail flowing.
	tools.entity_properties_window().open = false;
	CHECK(tools.needs_entity_detail(), "an open AI window alone wants the detail card");
	ai.open = false;
	tools.entity_properties_window().open = false;
	CHECK(!tools.needs_entity_detail(), "both panes closed wants none");
	ai.open = true;

	// A world-half-only card (no AI) formats no brain pane.
	tools.select_entity(0x1002);
	tools.set_entity_detail(detail_for(0x1002, 0, 0));
	CHECK(!ai.detail_valid(), "a card without the AI half is not a brain pane");
	CHECK(ai.detail_line_count() == 0, "...and formats nothing");

	tools.select_entity(0x3001);
	tools.set_entity_detail(ai_detail_for(0x3001));
	CHECK(ai.detail_valid(), "the brain card lands again");
	tools.clear_entity_selection();
	CHECK(!ai.detail_valid() && ai.detail_line_count() == 0,
			"clearing the selection clears the pane");
}

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
	test_entities_window_selects_the_picked_handle();
	test_entity_properties_window_card_and_attrib_toggles();
	test_entity_edits_gate_on_the_pushed_authority();
	test_weapon_window_formats_the_pushed_definition();
	test_weapon_window_accumulates_the_trace_delta();
	test_weapon_window_request_queue_and_gating();
	test_weapon_window_delay_edits_keep_the_other_legs_authoring();
	test_weapon_window_draws_its_panes();
	test_environment_window_formats_the_pushed_record();
	test_environment_request_queue();
	test_ai_window_formats_the_pushed_snapshot();
	test_ai_window_detail_pane_follows_the_selection();
	test_rays_window_formats_the_pushed_record();
	test_rays_request_queue();
	test_physics_window_formats_the_pushed_record();
	test_physics_request_queue();
	if (g_failures != 0) {
		std::printf("%d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("devtools_test: OK\n");
	return 0;
}
