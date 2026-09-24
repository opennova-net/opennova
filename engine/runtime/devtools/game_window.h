// The mandatory Game workspace window. Rendering the game texture and the
// Play/Interact request policy are layered onto this window through its narrow
// viewport binding; the window policy itself is engine-owned. Its toolbar
// carries the runtime transport (pause / step / resume, the script pause,
// leaving the world) as debug-control rows read through the control board,
// and a status readout (session, logic clock, frame rate) the embedder pushes.
#pragma once

#include <runtime/devtools/control_board.h>
#include <runtime/devtools/control_request.h>
#include <runtime/devtools/game_status_snapshot.h>
#include <runtime/devtools/imgui_pass.h>
#include <runtime/devtools/overlay_camera.h>

#include <deque>
#include <string>
#include <vector>

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
// binding null or install a fake. draw returns true when the game image was
// drawn at the cursor at exactly the requested size as the window's last
// item, which is where the overlays land.
class GameViewport {
public:
	virtual ~GameViewport() = default;
	virtual bool draw(int requested_width, int requested_height) = 0;
};

class GameWindow : public Window {
public:
	explicit GameWindow(ControlBoard &board) : board_(board) {}

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
	// The toolbar's transport rows: runtime_transport with "pause" / "step" /
	// "resume", runtime_wac_paused, and runtime_return_to_menu (the button
	// asks for confirmation first; this queues the confirmed request).
	void request_transport(const char *verb);
	void request_scripts_paused(bool paused);
	void request_return_to_menu();
	bool take_request(GameWindowRequest &request);
	bool take_control_request(ControlRequest &request);
	void reset_input_mode();
	void draw(ImGuiPass &pass, uint64_t frame_index) override;
	void wanted_controls(std::vector<const char *> &out) const override;

	// The status readout, by value; the formatted line is a test seam.
	void set_status(const GameStatusSnapshot &status);
	const std::string &status_text() const { return status_text_; }

	// The camera the overlays project through (an invalid record draws none).
	void set_overlay_camera(const OverlayCamera &camera) { overlay_camera_ = camera; }
	const OverlayCamera &overlay_camera() const { return overlay_camera_; }
	// The overlay pass over the image just drawn (the layout pass calls it;
	// public so a test can drive it over a fake image). Skipped while the
	// camera was built for another size (a resize in flight).
	void draw_overlays(ImGuiPass &pass, int image_width, int image_height);
	int overlay_draws() const { return overlay_draws_; }

private:
	void draw_toolbar();

	ControlBoard &board_;
	OverlayCamera overlay_camera_{};
	int overlay_draws_ = 0;
	GameStatusSnapshot status_{};
	std::string status_text_;
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
