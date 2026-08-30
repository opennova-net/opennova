// The game's dev tools (ADR 0039): the ImGui pass behind F3 with its mandatory
// Game surface, the Stats window (both open by default), the Entities window
// (closed by default; the pushed-record/typed-request channel, ADR 0042 d6),
// and ImGui's demo window (the docking/multi-viewport smoke test). Debug builds only
// (OPENNOVA_DEVTOOLS); the release GDExtension flavour compiles this out and
// its DevTools node is inert.
#pragma once

#include <runtime/devtools/frame_stats_board.h>
#include <runtime/devtools/imgui_pass.h>

namespace opennova::devtools {

class StatsWindow;
class GameWindow;
class GameViewport;
class EntitiesWindow;
enum class GameInputMode;
enum class GameWindowRequest;
struct DebugRequest;
struct EntityDirectorySnapshot;

class GameDevTools {
public:
	GameDevTools();

	ImGuiPass &pass() { return pass_; }
	const ImGuiPass &pass() const { return pass_; }
	StatsWindow &stats_window() { return *stats_window_; }
	const StatsWindow &stats_window() const { return *stats_window_; }
	EntitiesWindow &entities_window() { return *entities_window_; }
	const EntitiesWindow &entities_window() const { return *entities_window_; }
	void set_game_viewport(GameViewport *viewport);
	void set_game_play_available(bool available);
	void set_game_spectator_state(bool available, bool active);
	void set_game_input_mode(GameInputMode mode);
	void reset_game_input_mode();
	void request_game_escape();
	bool take_game_request(GameWindowRequest &request);

	// The board the Stats window reads (owned by the embedder; may be null).
	void set_frame_stats(FrameStatsBoard *board);

	// The Entities window's record/request channel (ADR 0042 d6). The
	// embedder pushes the directory by value (an invalid snapshot clears),
	// gated on needs_entity_directory (pass open && window open) so nobody
	// builds snapshots a closed window would drop, and drains the window's
	// typed debug requests into the engine-backed delegates.
	void set_entity_directory(EntityDirectorySnapshot snapshot);
	bool needs_entity_directory() const;
	bool take_debug_request(DebugRequest &request);

private:
	ImGuiPass pass_;
	GameWindow *game_window_ = nullptr;
	StatsWindow *stats_window_ = nullptr;
	EntitiesWindow *entities_window_ = nullptr;
};

}  // namespace opennova::devtools
