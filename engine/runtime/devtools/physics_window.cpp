#include <runtime/devtools/physics_window.h>

#include <imgui.h>

#include <cstdio>

namespace opennova::devtools {

namespace {

// The overlay's kind palette (godot/game/debug/collision_debug_view.gd
// HIT_KIND_COLORS, enum order) — the window doubles as the legend for the
// flashes drawn in-world; keep the two tables in sync.
constexpr float kKindColors[kContactKindCount][3] = {
	{1.0f, 0.35f, 0.15f}, // Projectile hit
	{1.0f, 0.4f, 0.7f},   // Knife hit
	{1.0f, 0.7f, 0.2f},   // Move contact
	{0.9f, 0.3f, 1.0f},   // Vehicle hull
	{0.75f, 0.6f, 0.4f},  // Terrain hit
	{0.3f, 0.9f, 1.0f},   // Water hit
};

}  // namespace

void PhysicsWindow::on_visibility(bool visible) {
	shown_ = visible;
	if (!visible) {
		// Drop the snapshot so a closed window holds nothing; the embedder's
		// needs_physics_snapshot gate stops the pushes on the same edge.
		snapshot_ = PhysicsSnapshot{};
		format_rows();
	}
}

void PhysicsWindow::set_snapshot(const PhysicsSnapshot &snapshot) {
	snapshot_ = snapshot;
	// Mirror the authoritative state into the edit controls: a click flips
	// locally and queues its request, the next push confirms it here.
	view_edit_ = snapshot_.view_shown;
	capture_edit_ = snapshot_.capturing;
	mask_edit_ = snapshot_.kind_mask;
	format_rows();
}

void PhysicsWindow::format_rows() {
	if (!snapshot_.valid) {
		for (std::string &row : rows_) row.clear();
		return;
	}
	for (int i = 0; i < kContactKindCount; ++i) {
		const PhysicsKindCount &c = snapshot_.kinds[static_cast<size_t>(i)];
		char buf[96];
		std::snprintf(buf, sizeof(buf), "%s: held %d / total %llu",
				c.name != nullptr ? c.name : "?", c.held,
				static_cast<unsigned long long>(c.total));
		rows_[static_cast<size_t>(i)] = buf;
	}
}

int PhysicsWindow::row_count() const {
	return snapshot_.valid ? kContactKindCount : 0;
}

const char *PhysicsWindow::row_text(int row) const {
	if (row < 0 || row >= row_count()) return "";
	return rows_[static_cast<size_t>(row)].c_str();
}

void PhysicsWindow::enqueue_request(const PhysicsRequest &request) {
	requests_.push_back(request);
}

bool PhysicsWindow::take_request(PhysicsRequest &request) {
	if (requests_.empty()) return false;
	request = requests_.front();
	requests_.pop_front();
	return true;
}

void PhysicsWindow::draw(ImGuiPass &pass, uint64_t frame_index) {
	(void)pass;
	(void)frame_index;
	if (!snapshot_.valid) {
		ImGui::TextUnformatted("No collision state pushed (load a mission).");
		return;
	}

	if (ImGui::Checkbox("Show collision", &view_edit_)) {
		enqueue_request({PhysicsRequest::Kind::SetViewShown, view_edit_ ? 1 : 0});
	}
	ImGui::SameLine();
	if (ImGui::Checkbox("Flash hits", &capture_edit_)) {
		enqueue_request({PhysicsRequest::Kind::SetCaptureEnabled, capture_edit_ ? 1 : 0});
	}
	ImGui::SameLine();
	ImGui::TextUnformatted(snapshot_.capturing ? "(capturing)" : "(idle)");
	ImGui::SameLine();
	if (ImGui::Button("Clear")) {
		enqueue_request({PhysicsRequest::Kind::Clear, 0});
	}
	ImGui::Text("logic tick %llu (%.2f s readings)",
			static_cast<unsigned long long>(snapshot_.logic_tick), kRefreshSeconds);
	ImGui::Text("boxes drawn %d", snapshot_.boxes_drawn);
	ImGui::Text("recent hits %d (inside the flash window)", snapshot_.recent);

	if (ImGui::SmallButton("All")) {
		mask_edit_ = kContactKindMaskAll;
		enqueue_request({PhysicsRequest::Kind::SetKindMask,
				static_cast<int32_t>(mask_edit_)});
	}
	ImGui::SameLine();
	if (ImGui::SmallButton("None")) {
		mask_edit_ = 0;
		enqueue_request({PhysicsRequest::Kind::SetKindMask, 0});
	}

	if (ImGui::BeginTable("contact_kinds", 2,
				ImGuiTableFlags_SizingStretchProp)) {
		const ImVec2 swatch_size(ImGui::GetFrameHeight(), ImGui::GetFrameHeight());
		for (int i = 0; i < kContactKindCount; ++i) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::PushID(i);
			bool on = (mask_edit_ & (1u << i)) != 0;
			if (ImGui::Checkbox("##draw", &on)) {
				mask_edit_ = on ? (mask_edit_ | (1u << i))
								: (mask_edit_ & ~(1u << i));
				enqueue_request({PhysicsRequest::Kind::SetKindMask,
						static_cast<int32_t>(mask_edit_)});
			}
			ImGui::SameLine();
			ImGui::ColorButton("##swatch",
					ImVec4(kKindColors[i][0], kKindColors[i][1],
							kKindColors[i][2], 1.0f),
					ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker,
					swatch_size);
			ImGui::SameLine();
			ImGui::TextUnformatted(snapshot_.kinds[static_cast<size_t>(i)].name);
			ImGui::TableNextColumn();
			ImGui::Text("held %d / total %llu",
					snapshot_.kinds[static_cast<size_t>(i)].held,
					static_cast<unsigned long long>(
							snapshot_.kinds[static_cast<size_t>(i)].total));
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
}

}  // namespace opennova::devtools
