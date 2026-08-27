// The game's dev tools (ADR 0039): the ImGui pass behind F3 with its mandatory
// Game surface, the Stats window (both open by default), and ImGui's demo
// window (the docking/multi-viewport smoke test). Debug builds only
// (OPENNOVA_DEVTOOLS); the release GDExtension flavour compiles this out and
// its DevTools node is inert.
#pragma once

#include <runtime/devtools/frame_stats_board.h>
#include <runtime/devtools/imgui_pass.h>

namespace opennova::devtools {

class StatsWindow;
class GameWindow;
class GameViewport;
enum class GameInputMode;
enum class GameWindowRequest;

class GameDevTools {
public:
	GameDevTools();

	ImGuiPass &pass() { return pass_; }
	const ImGuiPass &pass() const { return pass_; }
	StatsWindow &stats_window() { return *stats_window_; }
	const StatsWindow &stats_window() const { return *stats_window_; }
	GameWindow &game_window() { return *game_window_; }
	const GameWindow &game_window() const { return *game_window_; }

	void set_game_viewport(GameViewport *viewport);
	void set_game_play_available(bool available);
	void set_game_input_mode(GameInputMode mode);
	void reset_game_input_mode();
	void request_game_escape();
	bool take_game_request(GameWindowRequest &request);

	// The board the Stats window reads (owned by the embedder; may be null).
	void set_frame_stats(FrameStatsBoard *board);
	FrameStatsBoard *frame_stats() const { return frame_stats_; }

private:
	ImGuiPass pass_;
	GameWindow *game_window_ = nullptr;
	StatsWindow *stats_window_ = nullptr;
	FrameStatsBoard *frame_stats_ = nullptr;
};

}  // namespace opennova::devtools
