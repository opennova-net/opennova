// The Net window: the in-match network over the NetSnapshot the embedder
// pushes — the session (role, state, tick banking, the last frame's tick
// cost), the datagram traffic with its rates between pushes, the listen
// server's peers (round trip, the client's reported quality, ping strikes,
// the send holdoff, per-peer traffic), and a joiner's own link and admission
// state. The retail engine's network debug screen showed the frame rate, the
// CPU load and the local/remote quantum size [orig: Network_DrawDebugScreen
// @0x500EF0]; the header line carries the frame rate and the send holdoff
// (the quantum the joiner and the server exchange).
#pragma once

#include <runtime/devtools/control_board.h>
#include <runtime/devtools/control_request.h>
#include <runtime/devtools/imgui_pass.h>
#include <runtime/devtools/net_snapshot.h>

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace opennova::devtools {

class NetWindow : public Window {
public:
	static constexpr double kRefreshSeconds = 0.5;

	explicit NetWindow(ControlBoard &board) : board_(board) {}

	const char *title() const override { return "Net"; }
	MenuGroup menu_group() const override { return MenuGroup::Net; }
	WindowSizeHint preferred_size() const override { return {760.0f, 420.0f}; }
	void draw(ImGuiPass &pass, uint64_t frame_index) override;
	void on_visibility(bool visible) override;
	void wanted_controls(std::vector<const char *> &out) const override;

	void set_snapshot(NetSnapshot snapshot);
	bool snapshot_valid() const { return snapshot_.valid; }
	bool take_request(ControlRequest &request);

	// The formatted readings, for tests.
	const std::string &session_text() const { return session_; }
	const std::string &traffic_text() const { return traffic_; }
	int peer_count() const { return static_cast<int>(snapshot_.peers.size()); }

private:
	void format();

	ControlBoard &board_;
	NetSnapshot snapshot_{};
	NetSnapshot previous_{};
	std::string session_;
	std::string traffic_;
	std::deque<ControlRequest> requests_;
};

}  // namespace opennova::devtools
