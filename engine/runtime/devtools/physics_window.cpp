#include <runtime/devtools/physics_window.h>

#include <runtime/devtools/capture_table.h>
#include <runtime/devtools/ui_kit.h>

#include <imgui.h>

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
		rows_[static_cast<size_t>(i)] = capture_row_text(snapshot_.kinds[static_cast<size_t>(i)]);
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
	ui_kit::WrapRow layers;
	bool contacts_on = contacts_layer_.enabled();
	if (ui_kit::check(layers, "Show contacts in Game view", &contacts_on, contacts_layer_.tooltip())) {
		pass.set_overlay_enabled(contacts_layer_, contacts_on);
	}
	bool hitboxes_on = hitbox_layer_.enabled();
	if (ui_kit::check(layers, "Show hit meshes", &hitboxes_on, hitbox_layer_.tooltip())) {
		pass.set_overlay_enabled(hitbox_layer_, hitboxes_on);
	}
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
		ui_kit::empty_state("No collision state pushed (load a mission).");
		return;
	}

	ui_kit::WrapRow capture;
	const char *state = snapshot_.capturing ? "Capturing hits" : "Capture idle";
	capture.next(ui_kit::text_width(state));
	ImGui::TextUnformatted(state);
	ui_kit::tooltip("The contact capture records while this window shows.");
	if (ui_kit::tool(capture, "Clear", true, "Drop the captured contacts.")) {
		enqueue_request({PhysicsRequest::Kind::Clear, 0});
	}
	ImGui::Text("logic tick %llu (%.2f s readings)",
			static_cast<unsigned long long>(snapshot_.logic_tick), kRefreshSeconds);
	ImGui::Text("recent hits %d (inside the TTL window)", snapshot_.recent);

	if (capture_mask_table("contact_kinds", mask_edit_, kContactKindMaskAll, snapshot_.kinds,
				kContactKindColors, kContactKindCount)) {
		enqueue_request({PhysicsRequest::Kind::SetKindMask, static_cast<int32_t>(mask_edit_)});
	}
}

}  // namespace opennova::devtools
