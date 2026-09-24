// The Player window: the local player over the PlayerSnapshot the embedder
// pushes (world::inspect::local_player_report, by value) — the body, the
// view, the equipped weapon and the movement resolver's last capsule pass —
// and the player's debug rows: the first/third-person switches (through the
// control board), the map mode, look nudges, the viewmodel override, the
// deploy pick and crewing a vehicle.
#pragma once

#include <runtime/devtools/control_board.h>
#include <runtime/devtools/control_request.h>
#include <runtime/devtools/imgui_pass.h>
#include <runtime/world/inspect_local_player.h>

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace opennova::devtools {

struct PlayerSnapshot {
	bool valid = false;
	uint64_t logic_tick = 0;
	bool authority = false; // the world's owner (not a joiner)
	world::inspect::LocalPlayerReport report;
};

class PlayerWindow : public Window {
public:
	static constexpr double kRefreshSeconds = 0.25;
	static constexpr float kLookNudgePx = 32.0f;

	explicit PlayerWindow(ControlBoard &board) : board_(board) {}

	const char *title() const override { return "Player"; }
	MenuGroup menu_group() const override { return MenuGroup::World; }
	WindowSizeHint preferred_size() const override { return {520.0f, 520.0f}; }
	void draw(ImGuiPass &pass, uint64_t frame_index) override;
	void on_visibility(bool visible) override;
	void wanted_controls(std::vector<const char *> &out) const override;

	void set_snapshot(PlayerSnapshot snapshot);
	bool snapshot_valid() const { return snapshot_.valid; }

	// The rows with arguments this window builds itself.
	void request_look(float dx_px, float dy_px);
	void request_viewmodel_weapon(const std::string &weapon);
	void request_deploy_pick(int32_t zone);
	void request_crew_local_player(int32_t vehicle_ssn);
	bool take_request(ControlRequest &request);

	// The formatted body line, for tests.
	const std::string &body_text() const { return body_text_; }

private:
	void format();

	ControlBoard &board_;
	PlayerSnapshot snapshot_{};
	std::string body_text_;
	std::deque<ControlRequest> requests_;
	char viewmodel_edit_[64] = {};
	int deploy_zone_edit_ = 0;
	int crew_ssn_edit_ = 0;
};

}  // namespace opennova::devtools
