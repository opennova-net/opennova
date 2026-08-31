// The game's dev tools (ADR 0039): the ImGui pass behind F3 with its mandatory
// Game surface, the Stats window (both open by default), the Entities and
// Entity Properties windows (closed by default, opened by a world pick; the
// pushed-record/typed-request channel, ADR 0042 d6), the Environment, AI and
// Rays windows (closed by default, opened from the "Windows" menu), and ImGui's
// demo window (the docking/multi-viewport smoke test). Debug builds only
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
class EntityPropertiesWindow;
class EnvironmentWindow;
class AiWindow;
class RaysWindow;
enum class GameInputMode;
enum class GameWindowRequest;
struct DebugRequest;
struct EntityDirectorySnapshot;
struct EntityDetailSnapshot;
struct EnvironmentRequest;
struct EnvironmentSnapshot;
struct AiDebugSnapshot;
struct AiViewRequest;
struct RaysRequest;
struct RaysSnapshot;

class GameDevTools {
public:
	GameDevTools();

	ImGuiPass &pass() { return pass_; }
	const ImGuiPass &pass() const { return pass_; }
	StatsWindow &stats_window() { return *stats_window_; }
	const StatsWindow &stats_window() const { return *stats_window_; }
	EntitiesWindow &entities_window() { return *entities_window_; }
	const EntitiesWindow &entities_window() const { return *entities_window_; }
	EntityPropertiesWindow &entity_properties_window() { return *entity_properties_window_; }
	const EntityPropertiesWindow &entity_properties_window() const { return *entity_properties_window_; }
	EnvironmentWindow &environment_window() { return *environment_window_; }
	const EnvironmentWindow &environment_window() const { return *environment_window_; }
	AiWindow &ai_window() { return *ai_window_; }
	const AiWindow &ai_window() const { return *ai_window_; }
	RaysWindow &rays_window() { return *rays_window_; }
	const RaysWindow &rays_window() const { return *rays_window_; }
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
	// embedder pushes the directory by value (an invalid snapshot clears; it
	// carries the authority fact the edits gate on), gated on
	// needs_entity_directory (pass open && either entity window open) so
	// nobody builds snapshots no window would show, and drains the windows'
	// typed debug requests into the engine-backed delegates.
	void set_entity_directory(EntityDirectorySnapshot snapshot);
	bool needs_entity_directory() const;
	bool take_debug_request(DebugRequest &request);

	// The selection seam: the shell's world pick (a device event carrying
	// only the engine handle) opens and focuses the Entities window on that
	// row and the Entity Properties window on its card; the embedder pushes
	// the selected row's detail card (the engine card by value, an invalid
	// card clears) gated on needs_entity_detail (pass open && the Properties
	// window open && a selection). kInvalid = nothing selected.
	void select_entity(uint16_t handle);
	void clear_entity_selection();
	uint16_t selected_entity_handle() const;
	void set_entity_detail(EntityDetailSnapshot detail);
	bool needs_entity_detail() const;

	// The Environment window's record/request channel (the same shape): the
	// weather page record pushed by value on its cadence while shown, and
	// the typed weather commands drained into the engine command layer.
	void set_environment_snapshot(const EnvironmentSnapshot &snapshot);
	bool needs_environment_snapshot() const;
	bool take_environment_request(EnvironmentRequest &request);

	// The AI window's record/request channel (the same shape): the AI debug
	// join pushed by value on its cadence while shown, and the typed overlay
	// toggles drained into the shell's world-view seam (a request family
	// whose target is a device, not the engine command layer).
	void set_ai_debug(AiDebugSnapshot snapshot);
	bool needs_ai_debug() const;
	bool take_ai_view_request(AiViewRequest &request);

	// The Rays window's record/request channel (the same shape): the ray
	// capture's counts + filter state pushed by value on its cadence while
	// shown, and the typed filter/clear/view-toggle requests drained by the
	// embedder (filter and clear into the Simulation ray-debug seam; the view
	// toggle out to the shell that owns the 3D view).
	void set_rays_snapshot(const RaysSnapshot &snapshot);
	bool needs_rays_snapshot() const;
	bool take_rays_request(RaysRequest &request);


private:
	ImGuiPass pass_;
	GameWindow *game_window_ = nullptr;
	StatsWindow *stats_window_ = nullptr;
	EntitiesWindow *entities_window_ = nullptr;
	EntityPropertiesWindow *entity_properties_window_ = nullptr;
	EnvironmentWindow *environment_window_ = nullptr;
	AiWindow *ai_window_ = nullptr;
	RaysWindow *rays_window_ = nullptr;
};

}  // namespace opennova::devtools
