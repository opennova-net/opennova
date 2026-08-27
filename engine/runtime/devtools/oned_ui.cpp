#include <runtime/devtools/oned_ui.h>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

namespace opennova::devtools {

namespace {

ImVec4 status_color(OnedStatusKind kind) {
	switch (kind) {
		case OnedStatusKind::ERROR:
			return ImVec4(1.0f, 0.56f, 0.56f, 1.0f);
		case OnedStatusKind::WARN:
			return ImVec4(1.0f, 0.83f, 0.48f, 1.0f);
		case OnedStatusKind::INFO:
			break;
	}
	return ImVec4(0.72f, 0.75f, 0.80f, 1.0f);
}

// A field that commits on Enter or when focus leaves (the retired LineEdit's
// text_submitted / focus_exited edges).
bool committed_input(const char *label, const char *hint, std::string &value, bool *edited) {
	const bool submitted = ImGui::InputTextWithHint(label, hint, &value, ImGuiInputTextFlags_EnterReturnsTrue);
	if (edited != nullptr) {
		*edited = ImGui::IsItemEdited();
	}
	return submitted || ImGui::IsItemDeactivatedAfterEdit();
}

// A button with a reason: disabled and explaining itself while the reason is
// non-empty, otherwise carrying its usage tip.
bool action_button(const char *label, const ImVec2 &size, const std::string &block, const char *tip) {
	ImGui::BeginDisabled(!block.empty());
	const bool pressed = ImGui::Button(label, size);
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_ForTooltip)) {
		ImGui::SetTooltip("%s", block.empty() ? tip : block.c_str());
	}
	return pressed;
}

}  // namespace

// The single full-viewport surface.
class OnedUi::Surface : public Window {
public:
	explicit Surface(OnedUi &ui) : ui_(ui) { open = true; }

	const char *title() const override { return "ONED"; }
	bool owns_frame() const override { return true; }

	void draw(ImGuiPass &pass, uint64_t frame_index) override {
		(void)pass;
		(void)frame_index;
		const ImGuiViewport *viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(viewport->WorkPos);
		ImGui::SetNextWindowSize(viewport->WorkSize);
		const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
				ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
				ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings;
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28.0f, 24.0f));
		if (ImGui::Begin("##oned", nullptr, flags)) {
			draw_content();
		}
		ImGui::End();
		ImGui::PopStyleVar();
	}

private:
	void request(OnedAction action, int index = -1) { ui_.push_request(OnedRequest{action, index}); }

	void draw_content() {
		ImGui::TextUnformatted("ONED");
		ImGui::TextDisabled("Choose loose sources or a packed game and run it.");
		ImGui::Spacing();

		ImGui::SeparatorText("Game data");
		const float browse_width = ImGui::CalcTextSize("Browse...").x + ImGui::GetStyle().FramePadding.x * 2.0f;
		ImGui::SetNextItemWidth(-(browse_width + ImGui::GetStyle().ItemSpacing.x));
		bool edited = false;
		if (committed_input("##resource_dir", "Loose sources or packed game directory", ui_.resource_dir_, &edited)) {
			request(OnedAction::APPLY_RESOURCE_DIR);
		}
		if (edited) {
			request(OnedAction::EDIT_RESOURCE_DIR);
		}
		ImGui::SameLine();
		if (ImGui::Button("Browse...##resource")) {
			request(OnedAction::BROWSE_RESOURCE_DIR);
		}
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (ImGui::BeginCombo("##recent", "Recent resource directories...")) {
			for (int i = 0; i < static_cast<int>(ui_.recent_dirs_.size()); ++i) {
				ImGui::PushID(i);
				if (ImGui::Selectable(ui_.recent_dirs_[static_cast<size_t>(i)].c_str())) {
					request(OnedAction::SELECT_RECENT, i);
				}
				ImGui::PopID();
			}
			if (!ui_.recent_dirs_.empty()) {
				ImGui::Separator();
				if (ImGui::Selectable("Clear recent directories")) {
					request(OnedAction::CLEAR_RECENTS);
				}
			} else {
				ImGui::TextDisabled("(none yet)");
			}
			ImGui::EndCombo();
		}

		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted("Game");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(150.0f);
		if (committed_input("##game_code", "jo", ui_.game_code_, nullptr)) {
			request(OnedAction::COMMIT_PROFILE);
		}
		ImGui::SameLine();
		ImGui::TextUnformatted("Expansion");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (committed_input("##expansion", "Optional", ui_.expansion_, nullptr)) {
			request(OnedAction::COMMIT_PROFILE);
		}

		ImGui::SeparatorText("Retail install");
		ImGui::SetNextItemWidth(-(browse_width + ImGui::GetStyle().ItemSpacing.x));
		if (committed_input("##retail_dir", "Joint Operations install directory", ui_.retail_dir_, nullptr)) {
			request(OnedAction::APPLY_RETAIL_DIR);
		}
		ImGui::SameLine();
		if (ImGui::Button("Browse...##retail")) {
			request(OnedAction::BROWSE_RETAIL_DIR);
		}

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();
		const ImVec2 run_size(190.0f, 42.0f);
		if (action_button("Run OpenNova  F5", run_size, ui_.opennova_block_,
					"Run OpenNova directly from this loose or packed game-data directory (F5).")) {
			request(OnedAction::RUN_OPENNOVA);
		}
		ImGui::SameLine();
		if (action_button("Stage & Run Retail  F7", run_size, ui_.retail_block_,
					"Stage the selected game data and run the retail install (F7).")) {
			request(OnedAction::RUN_RETAIL);
		}
		ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 110.0f);
		static const std::string kNothingRunning = "Nothing is running.";
		if (action_button("Stop  F8", ImVec2(110.0f, 42.0f), ui_.running_ ? std::string() : kNothingRunning,
					"Stop the managed process (F8).")) {
			request(OnedAction::STOP);
		}

		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_Text, status_color(ui_.status_kind_));
		ImGui::TextWrapped("%s", ui_.status_text_.c_str());
		ImGui::PopStyleColor();
	}

	OnedUi &ui_;
};

OnedUi::OnedUi() : pass_(ImGuiPassOptions{/*dockspace=*/false, /*menu_bar=*/false,
							   /*escape_closes=*/false, /*start_open=*/true}) {
	pass_.register_window(std::make_unique<Surface>(*this));
}

OnedUi::~OnedUi() = default;

void OnedUi::set_readiness(std::string_view opennova_block, std::string_view retail_block, bool running) {
	opennova_block_ = opennova_block;
	retail_block_ = retail_block;
	running_ = running;
}

void OnedUi::set_status(std::string_view text, OnedStatusKind kind) {
	status_text_ = text;
	status_kind_ = kind;
}

bool OnedUi::take_request(OnedRequest &out) {
	if (requests_.empty()) {
		return false;
	}
	out = requests_.front();
	requests_.erase(requests_.begin());
	return true;
}

}  // namespace opennova::devtools
