#include <runtime/devtools/game_window.h>

#include <runtime/devtools/debug_control_ids.h>
#include <runtime/devtools/overlay_canvas.h>

#include <imgui.h>

#include <cstdio>

namespace opennova::devtools {

const char *status_role_label(StatusRole role) {
	switch (role) {
		case StatusRole::SinglePlayer:
			return "single player";
		case StatusRole::ListenServer:
			return "listen server";
		case StatusRole::Joiner:
			return "joiner";
		case StatusRole::DedicatedServer:
			return "dedicated server";
	}
	return "?";
}

const char *status_state_label(StatusState state) {
	switch (state) {
		case StatusState::Unloaded:
			return "unloaded";
		case StatusState::Connecting:
			return "connecting";
		case StatusState::Loading:
			return "loading";
		case StatusState::Running:
			return "running";
		case StatusState::Paused:
			return "paused";
		case StatusState::Stopping:
			return "stopping";
		case StatusState::Failed:
			return "failed";
	}
	return "?";
}

void GameWindow::set_status(const GameStatusSnapshot &status) {
	status_ = status;
	char buf[192];
	if (!status_.world) {
		std::snprintf(buf, sizeof(buf), "%.0f fps  %.1f ms (peak %.1f) | no world", status_.fps,
				status_.frame_ms, status_.frame_ms_peak);
	} else {
		char peers[32] = "";
		if (status_.role == StatusRole::ListenServer || status_.role == StatusRole::DedicatedServer) {
			std::snprintf(peers, sizeof(peers), ", %d peer%s", status_.peers,
					status_.peers == 1 ? "" : "s");
		}
		std::snprintf(buf, sizeof(buf), "%.0f fps  %.1f ms (peak %.1f) | tick %llu | %s%s, %s",
				status_.fps, status_.frame_ms, status_.frame_ms_peak,
				static_cast<unsigned long long>(status_.logic_tick), status_role_label(status_.role),
				peers, status_state_label(status_.state));
	}
	status_text_ = buf;
}

void GameWindow::request_transport(const char *verb) {
	control_requests_.push_back({control_id::kRuntimeTransport, {ControlArg::string(verb)}});
}

void GameWindow::request_scripts_paused(bool paused) {
	control_requests_.push_back({control_id::kRuntimeWacPaused, {ControlArg::boolean(paused)}});
	board_.set_local_value(control_id::kRuntimeWacPaused, ControlArg::boolean(paused));
}

void GameWindow::request_return_to_menu() {
	control_requests_.push_back({control_id::kRuntimeReturnToMenu, {}});
}

void GameWindow::wanted_controls(std::vector<const char *> &out) const {
	out.push_back(control_id::kRuntimeTransport);
	out.push_back(control_id::kRuntimeWacPaused);
	out.push_back(control_id::kRuntimeReturnToMenu);
}

void GameWindow::draw_toolbar() {
	// The transport row's writability is the table's own verdict (a network
	// role is refused pause/step natively; the reason is the tooltip).
	const ControlState *transport = board_.state(control_id::kRuntimeTransport);
	const bool transport_ok = transport != nullptr && transport->writable;
	const auto transport_button = [&](const char *label, const char *verb, bool enabled) {
		ImGui::BeginDisabled(!transport_ok || !enabled);
		if (ImGui::Button(label)) request_transport(verb);
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_ForTooltip)) {
			if (!transport_ok && transport != nullptr && !transport->reason.empty()) {
				ImGui::SetTooltip("%s", transport->reason.c_str());
			} else if (status_.transport_locked && std::string(verb) != "resume") {
				ImGui::SetTooltip("A network session keeps ticking: pause and step are refused.");
			}
		}
		ImGui::SameLine();
	};
	transport_button(status_.playing ? "Pause" : "Paused", "pause", status_.playing);
	transport_button("Step", "step", !status_.playing);
	transport_button("Resume", "resume", true);

	const ControlState *scripts = board_.state(control_id::kRuntimeWacPaused);
	bool scripts_paused = scripts != nullptr && scripts->has_value && scripts->value.b;
	ImGui::BeginDisabled(scripts == nullptr || !scripts->writable);
	if (ImGui::Checkbox("Scripts paused", &scripts_paused)) request_scripts_paused(scripts_paused);
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_ForTooltip)) {
		ImGui::SetTooltip("Pause the mission's WAC scripts while the rest of the world runs.");
	}
	ImGui::SameLine();

	const ControlState *leave = board_.state(control_id::kRuntimeReturnToMenu);
	ImGui::BeginDisabled(leave == nullptr || !leave->writable || !status_.world);
	if (ImGui::Button("Leave...")) ImGui::OpenPopup("leave_world");
	ImGui::EndDisabled();
	if (ImGui::BeginPopup("leave_world")) {
		ImGui::TextUnformatted("Leave this world and return to the menu?");
		if (ImGui::Button("Leave")) {
			request_return_to_menu();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
	ImGui::SameLine();
	ImGui::TextDisabled("%s", status_text_.c_str());
}

void GameWindow::set_play_available(bool available) {
	if (available == play_available_) {
		return;
	}
	play_available_ = available;
	if (!play_available_ && input_mode_ == GameInputMode::Play) {
		requests_.push_back(GameWindowRequest::EnterInteract);
	}
}

void GameWindow::request_enter_play() {
	if (play_available_ && input_mode_ == GameInputMode::Interact) {
		requests_.push_back(GameWindowRequest::EnterPlay);
	}
}

void GameWindow::set_spectator_state(bool available, bool active) {
	spectator_available_ = available;
	spectator_active_ = active;
}

void GameWindow::request_spectator(bool active) {
	if (!spectator_available_ || active == spectator_active_) {
		return;
	}
	control_requests_.push_back({control_id::kLocalSpectator, {ControlArg::boolean(active)}});
}

void GameWindow::request_escape() {
	escape_already_handled_ = true;
	requests_.push_back(input_mode_ == GameInputMode::Play
			? GameWindowRequest::EnterInteract
			: GameWindowRequest::CloseTools);
}

bool GameWindow::take_request(GameWindowRequest &request) {
	if (requests_.empty()) {
		return false;
	}
	request = requests_.front();
	requests_.pop_front();
	return true;
}

bool GameWindow::take_control_request(ControlRequest &request) {
	if (control_requests_.empty()) {
		return false;
	}
	request = control_requests_.front();
	control_requests_.pop_front();
	return true;
}

void GameWindow::reset_input_mode() {
	input_mode_ = GameInputMode::Interact;
	escape_already_handled_ = false;
	requests_.clear();
	control_requests_.clear();
}

void GameWindow::draw(ImGuiPass &pass, uint64_t frame_index) {
	(void)pass;
	(void)frame_index;
	const bool can_enter_play = play_available_ && input_mode_ == GameInputMode::Interact;
	ImGui::BeginDisabled(!can_enter_play);
	if (ImGui::Button(input_mode_ == GameInputMode::Play ? "Playing" : "Play")) {
		request_enter_play();
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::TextUnformatted(input_mode_ == GameInputMode::Play ? "Play" : "Interact");
	ImGui::SameLine();
	bool spectator = spectator_active_;
	ImGui::BeginDisabled(!spectator_available_);
	if (ImGui::Checkbox("Spectator", &spectator)) {
		request_spectator(spectator);
	}
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
		ImGui::SetTooltip(spectator_available_
				? "Detach the local player and unlock the free camera. Enter Play, then hold right mouse and use WASD/Q/E to fly."
				: "Spectator switching requires a local authority player.");
	}
	draw_toolbar();
	ImGui::Separator();
	if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::GetIO().WantTextInput &&
			!escape_already_handled_) {
		requests_.push_back(input_mode_ == GameInputMode::Play
				? GameWindowRequest::EnterInteract
				: GameWindowRequest::CloseTools);
	}
	escape_already_handled_ = false;
	if (viewport_ == nullptr) {
		return;
	}
	const ImVec2 available = ImGui::GetContentRegionAvail();
	const int width = available.x >= 1.0f ? static_cast<int>(available.x) : 1;
	const int height = available.y >= 1.0f ? static_cast<int>(available.y) : 1;
	if (viewport_->draw(width, height)) {
		draw_overlays(pass, width, height);
	}
}

void GameWindow::draw_overlays(ImGuiPass &pass, int image_width, int image_height) {
	if (!overlay_camera_.valid || !pass.any_overlay_enabled() ||
			overlay_camera_.viewport_width != image_width ||
			overlay_camera_.viewport_height != image_height) {
		return;
	}
	const ImVec2 min = ImGui::GetItemRectMin();
	const ImVec2 max = ImGui::GetItemRectMax();
	ImDrawList *draw_list = ImGui::GetWindowDrawList();
	draw_list->PushClipRect(min, max, true);
	OverlayRect rect;
	rect.min_x = min.x;
	rect.min_y = min.y;
	rect.max_x = max.x;
	rect.max_y = max.y;
	OverlayCanvas canvas(draw_list, overlay_camera_, rect);
	pass.draw_overlays(canvas);
	draw_list->PopClipRect();
	++overlay_draws_;
}

}  // namespace opennova::devtools
