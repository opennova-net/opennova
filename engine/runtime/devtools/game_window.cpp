#include <runtime/devtools/game_window.h>

#include <imgui.h>

namespace opennova::devtools {

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

void GameWindow::reset_input_mode() {
	input_mode_ = GameInputMode::Interact;
	escape_already_handled_ = false;
	requests_.clear();
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
	viewport_->draw(width, height);
}

}  // namespace opennova::devtools
