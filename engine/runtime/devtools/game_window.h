// The mandatory Game workspace window. Rendering the game texture and the
// Play/Interact request policy are layered onto this window through its narrow
// viewport binding; the window policy itself is engine-owned.
#pragma once

#include <runtime/devtools/control_request.h>
#include <runtime/devtools/imgui_pass.h>

#include <deque>

namespace opennova::devtools {

enum class GameInputMode {
	Interact,
	Play,
};

// The window's own input-mode policy requests; the spectator toggle is a
// debug-control row (local_spectator) and leaves as a ControlRequest instead.
enum class GameWindowRequest {
	EnterPlay,
	EnterInteract,
	CloseTools,
};

// The only seam between the engine-owned window and a rendering device. The
// Godot binding resizes and draws its SubViewport; engine-only runs leave the
// binding null or install a fake.
class GameViewport {
public:
	virtual ~GameViewport() = default;
	virtual void draw(int requested_width, int requested_height) = 0;
};

class GameWindow : public Window {
public:
	const char *title() const override { return "Game"; }
	MenuGroup menu_group() const override { return MenuGroup::Workspace; }
	bool is_closeable() const override { return false; }
	bool is_collapsible() const override { return false; }
	bool is_scrollable() const override { return false; }
	InitialDockPlacement initial_dock_placement() const override {
		return InitialDockPlacement::Center;
	}
	void set_viewport(GameViewport *viewport) { viewport_ = viewport; }
	void set_play_available(bool available);
	bool play_available() const { return play_available_; }
	void set_input_mode(GameInputMode mode) { input_mode_ = mode; }
	GameInputMode input_mode() const { return input_mode_; }
	void set_spectator_state(bool available, bool active);
	bool spectator_available() const { return spectator_available_; }
	bool spectator_active() const { return spectator_active_; }
	// The spectator checkbox: one local_spectator control request (the
	// debug-control table's row, so the same authority gate as MCP decides),
	// queued only while the shell reports the transition available.
	void request_spectator(bool active);
	void request_enter_play();
	void request_escape();
	bool take_request(GameWindowRequest &request);
	bool take_control_request(ControlRequest &request);
	void reset_input_mode();
	void draw(ImGuiPass &pass, uint64_t frame_index) override;

private:
	GameViewport *viewport_ = nullptr;
	GameInputMode input_mode_ = GameInputMode::Interact;
	bool play_available_ = false;
	bool spectator_available_ = false;
	bool spectator_active_ = false;
	bool escape_already_handled_ = false;
	std::deque<GameWindowRequest> requests_;
	std::deque<ControlRequest> control_requests_;
};

}  // namespace opennova::devtools
