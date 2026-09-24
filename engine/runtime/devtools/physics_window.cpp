#include <runtime/devtools/physics_window.h>

#include <imgui.h>

#include <cstdio>

namespace opennova::devtools {


void PhysicsWindow::on_visibility(bool visible) {
	if (!visible) {
		// Drop the snapshot so a closed window holds nothing; the embedder's
		// needs_physics_snapshot gate stops the pushes on the same edge.
		snapshot_ = PhysicsSnapshot{};
		format_rows();
	}
}

void PhysicsWindow::set_snapshot(const PhysicsSnapshot &snapshot) {
	snapshot_ = snapshot;
	// Mirror the authoritative mask into the checkboxes: a click flips
	// locally and queues its request, the next push confirms it here.
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
	(void)frame_index;
	bool contacts_on = contacts_layer_.enabled();
	if (ImGui::Checkbox("Show contacts in Game view", &contacts_on)) {
		pass.set_overlay_enabled(contacts_layer_, contacts_on);
	}
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", contacts_layer_.tooltip());
	ImGui::SameLine();
	bool hitboxes_on = hitbox_layer_.enabled();
	if (ImGui::Checkbox("Show hit meshes", &hitboxes_on)) pass.set_overlay_enabled(hitbox_layer_, hitboxes_on);
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", hitbox_layer_.tooltip());
	if (hitbox_record_.valid) {
		size_t faces = 0;
		int64_t authored = 0;
		for (const auto &e : hitbox_record_.report.entities) {
			faces += e.faces.size();
			authored += e.face_total;
		}
		ImGui::Text("hit meshes: %d bodies, %zu / %lld faces drawn, %d person sections",
				static_cast<int>(hitbox_record_.report.entities.size()), faces,
				static_cast<long long>(authored), static_cast<int>(hitbox_record_.report.organics.size()));
	}
	if (!snapshot_.valid) {
		ImGui::TextUnformatted("No collision state pushed (load a mission).");
		return;
	}

	ImGui::TextUnformatted(snapshot_.capturing ? "Capturing hits" : "Capture idle");
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("The contact capture records while this window shows.");
	}
	ImGui::SameLine();
	if (ImGui::Button("Clear")) {
		enqueue_request({PhysicsRequest::Kind::Clear, 0});
	}
	ImGui::Text("logic tick %llu (%.2f s readings)",
			static_cast<unsigned long long>(snapshot_.logic_tick), kRefreshSeconds);
	ImGui::Text("recent hits %d (inside the TTL window)", snapshot_.recent);

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
					ImVec4(kContactKindColors[i][0], kContactKindColors[i][1],
							kContactKindColors[i][2], 1.0f),
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
