// The mandatory Game workspace window. Rendering the game texture and the
// Play/Interact request policy are layered onto this window through its narrow
// viewport binding; the window policy itself is engine-owned.
#pragma once

#include <runtime/devtools/imgui_pass.h>

#include <deque>

namespace opennova::devtools {

enum class GameInputMode {
	Interact,
	Play,
};

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
	bool is_closeable() const override { return false; }
	bool is_undockable() const override { return false; }
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
	void request_enter_play();
	void request_escape();
	bool take_request(GameWindowRequest &request);
	void reset_input_mode();
	void draw(ImGuiPass &pass, uint64_t frame_index) override;

private:
	GameViewport *viewport_ = nullptr;
	GameInputMode input_mode_ = GameInputMode::Interact;
	bool play_available_ = false;
	bool escape_already_handled_ = false;
	std::deque<GameWindowRequest> requests_;
};

}  // namespace opennova::devtools
