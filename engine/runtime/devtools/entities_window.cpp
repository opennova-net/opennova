#include <runtime/devtools/entities_window.h>

#include <imgui.h>

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <utility>

namespace opennova::devtools {

namespace {

template <size_t N>
void set_text(std::array<char, N> &dst, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(dst.data(), N, fmt, args);
	va_end(args);
}

// Case-insensitive substring (the filter box; names are plain ASCII item /
// mission identifiers).
bool contains_ci(const char *haystack, const char *needle) {
	if (needle[0] == '\0') {
		return true;
	}
	const size_t needle_len = std::strlen(needle);
	for (const char *h = haystack; *h != '\0'; ++h) {
		size_t i = 0;
		while (i < needle_len && h[i] != '\0' &&
				std::tolower(static_cast<unsigned char>(h[i])) ==
						std::tolower(static_cast<unsigned char>(needle[i]))) {
			++i;
		}
		if (i == needle_len) {
			return true;
		}
	}
	return false;
}

}  // namespace

void EntitiesWindow::on_visibility(bool visible) {
	shown_ = visible;
	if (!visible) {
		// Drop the snapshot so a closed window holds nothing; the embedder's
		// needs_entity_directory gate stops the pushes on the same edge.
		snapshot_ = EntityDirectorySnapshot{};
		format_rows();
	}
}

void EntitiesWindow::set_directory(EntityDirectorySnapshot snapshot) {
	snapshot_ = std::move(snapshot);
	if (!snapshot_.valid) {
		snapshot_.rows.clear();
	}
	format_rows();
}

void EntitiesWindow::enqueue_request(const DebugRequest &request) {
	requests_.push_back(request);
}

bool EntitiesWindow::take_request(DebugRequest &request) {
	if (requests_.empty()) {
		return false;
	}
	request = requests_.front();
	requests_.pop_front();
	return true;
}

void EntitiesWindow::set_filter(const char *text) {
	std::snprintf(filter_.data(), filter_.size(), "%s", text != nullptr ? text : "");
	apply_filter();
}

void EntitiesWindow::format_rows() {
	texts_.clear();
	texts_.reserve(snapshot_.rows.size());
	// Keep the selection across pushes by wire handle (the directory reorders
	// as entities die and spawn).
	const uint16_t selected_handle = selected_row() != nullptr
			? selected_row()->wire_handle
			: 0;
	int reselected = -1;
	for (size_t i = 0; i < snapshot_.rows.size(); ++i) {
		const world::inspect::EntityRow &row = snapshot_.rows[i];
		RowText text;
		text.name = row.name.empty() ? "(unnamed)" : row.name;
		if (row.ai_index >= 0) {
			set_text(text.ai, "%d", row.ai_index);
		} else {
			set_text(text.ai, "-");
		}
		set_text(text.net_id, "%d", row.net_id);
		set_text(text.team, "%d", row.team);
		set_text(text.health, "%d", row.health);
		set_text(text.alive, "%s", row.alive ? "yes" : "no");
		set_text(text.pos, "%.1f %.1f %.1f", row.mission_position.x,
				row.mission_position.y, row.mission_position.z);
		texts_.push_back(std::move(text));
		if (selected_handle != 0 && row.wire_handle == selected_handle && reselected < 0) {
			reselected = static_cast<int>(i);
		}
	}
	selected_ = reselected;
	apply_filter();
}

void EntitiesWindow::apply_filter() {
	filtered_.clear();
	for (size_t i = 0; i < snapshot_.rows.size(); ++i) {
		const RowText &text = texts_[i];
		if (contains_ci(text.name.c_str(), filter_.data()) ||
				contains_ci(text.net_id.data(), filter_.data())) {
			filtered_.push_back(static_cast<int>(i));
		}
	}
}

void EntitiesWindow::select_row(int snapshot_index) {
	selected_ = snapshot_index;
	const world::inspect::EntityRow *row = selected_row();
	if (row == nullptr) {
		return;
	}
	// Seed the action edits from the row so "apply" without a touch is a
	// no-op-shaped write, not a zero.
	health_edit_ = row->health;
	pos_edit_[0] = row->mission_position.x;
	pos_edit_[1] = row->mission_position.y;
	pos_edit_[2] = row->mission_position.z;
}

const world::inspect::EntityRow *EntitiesWindow::selected_row() const {
	if (selected_ < 0 || selected_ >= static_cast<int>(snapshot_.rows.size())) {
		return nullptr;
	}
	return &snapshot_.rows[static_cast<size_t>(selected_)];
}

int EntitiesWindow::row_count() const {
	return static_cast<int>(filtered_.size());
}

const char *EntitiesWindow::row_name(int row) const {
	return (row >= 0 && row < row_count()) ? texts_[static_cast<size_t>(filtered_[static_cast<size_t>(row)])].name.c_str() : "";
}

const char *EntitiesWindow::row_ai(int row) const {
	return (row >= 0 && row < row_count()) ? texts_[static_cast<size_t>(filtered_[static_cast<size_t>(row)])].ai.data() : "";
}

const char *EntitiesWindow::row_net_id(int row) const {
	return (row >= 0 && row < row_count()) ? texts_[static_cast<size_t>(filtered_[static_cast<size_t>(row)])].net_id.data() : "";
}

const char *EntitiesWindow::row_team(int row) const {
	return (row >= 0 && row < row_count()) ? texts_[static_cast<size_t>(filtered_[static_cast<size_t>(row)])].team.data() : "";
}

const char *EntitiesWindow::row_health(int row) const {
	return (row >= 0 && row < row_count()) ? texts_[static_cast<size_t>(filtered_[static_cast<size_t>(row)])].health.data() : "";
}

const char *EntitiesWindow::row_alive(int row) const {
	return (row >= 0 && row < row_count()) ? texts_[static_cast<size_t>(filtered_[static_cast<size_t>(row)])].alive.data() : "";
}

const char *EntitiesWindow::row_pos(int row) const {
	return (row >= 0 && row < row_count()) ? texts_[static_cast<size_t>(filtered_[static_cast<size_t>(row)])].pos.data() : "";
}

void EntitiesWindow::draw_selected_actions() {
	ImGui::Separator();
	const world::inspect::EntityRow *row = selected_row();
	if (row == nullptr) {
		ImGui::TextUnformatted("Select a row to edit it.");
		return;
	}
	ImGui::Text("Selected: %s (ssn %d)",
			row->name.empty() ? "(unnamed)" : row->name.c_str(), row->net_id);
	// The edit seams key on the AI brain (editable = brain + live registry
	// slot); a brainless row still offers the local-player teleport below.
	ImGui::BeginDisabled(!row->editable);
	ImGui::SetNextItemWidth(96.0f);
	ImGui::InputInt("##entity_health", &health_edit_);
	ImGui::SameLine();
	if (ImGui::Button("Set health")) {
		DebugRequest request;
		request.kind = DebugRequest::Kind::SetEntityHealth;
		request.target.packed = row->wire_handle;
		request.health = health_edit_;
		enqueue_request(request);
	}
	ImGui::SetNextItemWidth(240.0f);
	ImGui::InputFloat3("##entity_pos", pos_edit_, "%.1f");
	ImGui::SameLine();
	if (ImGui::Button("Set position")) {
		DebugRequest request;
		request.kind = DebugRequest::Kind::SetEntityPosition;
		request.target.packed = row->wire_handle;
		request.pos[0] = pos_edit_[0];
		request.pos[1] = pos_edit_[1];
		request.pos[2] = pos_edit_[2];
		enqueue_request(request);
	}
	ImGui::EndDisabled();
	ImGui::SetNextItemWidth(64.0f);
	ImGui::InputFloat("yaw", &yaw_edit_, 0.0f, 0.0f, "%.0f");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(64.0f);
	ImGui::InputFloat("pitch", &pitch_edit_, 0.0f, 0.0f, "%.0f");
	ImGui::SameLine();
	if (ImGui::Button("Teleport player here")) {
		DebugRequest request;
		request.kind = DebugRequest::Kind::TeleportLocalPlayer;
		request.pos[0] = pos_edit_[0];
		request.pos[1] = pos_edit_[1];
		request.pos[2] = pos_edit_[2];
		request.yaw = yaw_edit_;
		request.pitch = pitch_edit_;
		enqueue_request(request);
	}
}

void EntitiesWindow::draw(ImGuiPass &pass, uint64_t frame_index) {
	(void)pass;
	(void)frame_index;
	if (!snapshot_.valid) {
		ImGui::TextUnformatted("No entity directory pushed (load a mission).");
		return;
	}
	ImGui::Text("%d of %d entities | logic tick %llu (%.1f s readings)",
			row_count(), static_cast<int>(snapshot_.rows.size()),
			static_cast<unsigned long long>(snapshot_.logic_tick), kRefreshSeconds);
	ImGui::SetNextItemWidth(-64.0f);
	if (ImGui::InputText("Filter", filter_.data(), filter_.size())) {
		apply_filter();
	}

	// Leave room under the table for the selected-row action strip.
	const float actions_height = ImGui::GetFrameHeightWithSpacing() * 4.0f;
	const ImGuiTableFlags table_flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
			ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
	if (ImGui::BeginTable("entity_rows", 7, table_flags, ImVec2(0.0f, -actions_height))) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 3.0f);
		ImGui::TableSetupColumn("AI", ImGuiTableColumnFlags_WidthFixed, 36.0f);
		ImGui::TableSetupColumn("SSN", ImGuiTableColumnFlags_WidthFixed, 48.0f);
		ImGui::TableSetupColumn("Team", ImGuiTableColumnFlags_WidthFixed, 40.0f);
		ImGui::TableSetupColumn("HP", ImGuiTableColumnFlags_WidthFixed, 48.0f);
		ImGui::TableSetupColumn("Alive", ImGuiTableColumnFlags_WidthFixed, 40.0f);
		ImGui::TableSetupColumn("Position", ImGuiTableColumnFlags_WidthStretch, 2.0f);
		ImGui::TableHeadersRow();
		for (const int index : filtered_) {
			const RowText &text = texts_[static_cast<size_t>(index)];
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::PushID(index);
			if (ImGui::Selectable(text.name.c_str(), index == selected_,
						ImGuiSelectableFlags_SpanAllColumns)) {
				select_row(index);
			}
			ImGui::PopID();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(text.ai.data());
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(text.net_id.data());
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(text.team.data());
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(text.health.data());
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(text.alive.data());
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(text.pos.data());
		}
		ImGui::EndTable();
	}
	draw_selected_actions();
}

}  // namespace opennova::devtools
