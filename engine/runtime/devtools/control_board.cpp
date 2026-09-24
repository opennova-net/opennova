#include <runtime/devtools/control_board.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <string>

namespace opennova::devtools {

void ControlBoard::set_catalog(std::vector<ControlSpec> catalog) {
	catalog_ = std::move(catalog);
	index_.clear();
	for (size_t i = 0; i < catalog_.size(); ++i) {
		index_[catalog_[i].id] = i;
	}
}

const ControlSpec *ControlBoard::spec(const char *id) const {
	const auto it = index_.find(id);
	return it == index_.end() ? nullptr : &catalog_[it->second];
}

void ControlBoard::set_states(const std::vector<ControlState> &states) {
	for (const ControlState &state : states) {
		states_[state.id] = state;
	}
}

const ControlState *ControlBoard::state(const char *id) const {
	const auto it = states_.find(id);
	return it == states_.end() ? nullptr : &it->second;
}

void ControlBoard::set_local_value(const char *id, const ControlArg &value) {
	ControlState &state = states_[id];
	state.id = id;
	state.value = value;
	state.has_value = true;
}

namespace {

double number_of(const ControlArg &value) {
	switch (value.kind) {
		case ControlArg::Kind::Int:
			return static_cast<double>(value.i);
		case ControlArg::Kind::Float:
			return value.f;
		case ControlArg::Kind::Bool:
			return value.b ? 1.0 : 0.0;
		default:
			return 0.0;
	}
}

void row_tooltip(const ControlSpec &spec, const ControlState *state) {
	if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_ForTooltip)) {
		return;
	}
	const bool refused = state == nullptr || !state->writable;
	if (spec.tooltip.empty() && (!refused || state == nullptr || state->reason.empty())) {
		return;
	}
	ImGui::BeginTooltip();
	if (!spec.tooltip.empty()) {
		ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
		ImGui::TextUnformatted(spec.tooltip.c_str());
		ImGui::PopTextWrapPos();
	}
	if (refused && state != nullptr && !state->reason.empty()) {
		ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "%s", state->reason.c_str());
	}
	ImGui::EndTooltip();
}

}  // namespace

bool draw_control(ControlBoard &board, const char *id, std::deque<ControlRequest> &queue,
		const char *label) {
	const ControlSpec *spec = board.spec(id);
	if (spec == nullptr) {
		ImGui::TextDisabled("%s (no such control)", id);
		return false;
	}
	const ControlState *state = board.state(id);
	const std::string text = (label != nullptr ? std::string(label) : spec->label) + "##" + spec->id;
	const bool writable = state != nullptr && state->writable;
	bool queued = false;
	ImGui::BeginDisabled(!writable);
	switch (spec->kind) {
		case ControlKind::Check: {
			bool on = state != nullptr && state->has_value && number_of(state->value) != 0.0;
			if (ImGui::Checkbox(text.c_str(), &on)) {
				queue.push_back({id, {ControlArg::boolean(on)}});
				board.set_local_value(id, ControlArg::boolean(on));
				queued = true;
			}
			break;
		}
		case ControlKind::Slider: {
			float &edit = board.slider_edit(id);
			// ImGui also releases the active ID when a widget disappears
			// (F3 closes, its window hides, or its section collapses). A
			// separate editing latch would miss that deactivation frame.
			if (ImGui::GetActiveID() != ImGui::GetID(text.c_str())) {
				edit = state != nullptr && state->has_value
						? static_cast<float>(number_of(state->value))
						: static_cast<float>(spec->minimum);
			}
			const bool integral = spec->step >= 1.0;
			ImGui::SliderFloat(text.c_str(), &edit, static_cast<float>(spec->minimum),
					static_cast<float>(spec->maximum), integral ? "%.0f" : "%.2f");
			if (ImGui::IsItemDeactivatedAfterEdit()) {
				queue.push_back({id, {ControlArg::number(edit)}});
				board.set_local_value(id, ControlArg::number(edit));
				queued = true;
			}
			break;
		}
		case ControlKind::Enum: {
			const int current = state != nullptr && state->has_value
					? static_cast<int>(number_of(state->value))
					: -1;
			const char *preview = current >= 0 && current < static_cast<int>(spec->choices.size())
					? spec->choices[static_cast<size_t>(current)].c_str()
					: "";
			if (ImGui::BeginCombo(text.c_str(), preview)) {
				for (int i = 0; i < static_cast<int>(spec->choices.size()); ++i) {
					const bool selected = i == current;
					if (ImGui::Selectable(spec->choices[static_cast<size_t>(i)].c_str(), selected) &&
							!selected) {
						queue.push_back({id, {ControlArg::integer(i)}});
						board.set_local_value(id, ControlArg::integer(i));
						queued = true;
					}
					if (selected) ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
			break;
		}
		case ControlKind::Action: {
			// Actions with arguments are the windows' own widgets; the board
			// draws only the argument-less press.
			if (ImGui::Button(text.c_str())) {
				queue.push_back({id, {}});
				queued = true;
			}
			break;
		}
	}
	ImGui::EndDisabled();
	row_tooltip(*spec, state);
	return queued;
}

}  // namespace opennova::devtools
