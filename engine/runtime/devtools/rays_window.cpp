#include <runtime/devtools/rays_window.h>
#include <base/io/tick_rate.h>

#include <imgui.h>

#include <cstdio>

namespace opennova::devtools {

namespace {

// The ray-category palette (the engine's, in RayDebugCategory enum order):
// the swatch beside each category row.
constexpr float kCategoryColors[kRayCategoryCount][3] = {
	{0.7f, 0.7f, 0.7f},    // Uncategorized
	{1.0f, 0.35f, 0.15f},  // Projectile
	{1.0f, 0.4f, 0.7f},    // Knife
	{1.0f, 0.7f, 0.2f},    // Throwable
	{0.95f, 0.95f, 0.25f}, // AI LOS
	{0.7f, 0.4f, 1.0f},    // Replication LOS
	{0.2f, 0.9f, 0.75f},   // Script LOS
	{1.0f, 0.5f, 0.4f},    // Explosion LOS
	{0.75f, 0.6f, 0.4f},   // Ground probe
	{0.4f, 0.75f, 1.0f},   // Camera iris
	{0.25f, 0.45f, 1.0f},  // Render occlusion
	{1.0f, 0.85f, 0.3f},   // Sun visibility
	{0.35f, 1.0f, 0.45f},  // Sound occlusion
	{0.3f, 0.9f, 1.0f},    // Precipitation
	{1.0f, 1.0f, 1.0f},    // Pick
};

}  // namespace

void RaysWindow::on_visibility(bool visible) {
	shown_ = visible;
	if (!visible) {
		// Drop the snapshot so a closed window holds nothing; the embedder's
		// needs_rays_snapshot gate stops the pushes on the same edge.
		snapshot_ = RaysSnapshot{};
		format_rows();
	}
}

void RaysWindow::set_snapshot(const RaysSnapshot &snapshot) {
	snapshot_ = snapshot;
	// Mirror the authoritative state into the edit controls: a click flips
	// locally and queues its request, the next push confirms it here.
	mask_edit_ = snapshot_.category_mask;
	ttl_edit_ = snapshot_.ttl_ticks;
	format_rows();
}

void RaysWindow::format_rows() {
	if (!snapshot_.valid) {
		for (std::string &row : rows_) row.clear();
		return;
	}
	for (int i = 0; i < kRayCategoryCount; ++i) {
		const RaysCategoryCount &c = snapshot_.categories[static_cast<size_t>(i)];
		char buf[96];
		std::snprintf(buf, sizeof(buf), "%s: held %d / total %llu",
				c.name != nullptr ? c.name : "?", c.held,
				static_cast<unsigned long long>(c.total));
		rows_[static_cast<size_t>(i)] = buf;
	}
}

int RaysWindow::row_count() const {
	return snapshot_.valid ? kRayCategoryCount : 0;
}

const char *RaysWindow::row_text(int row) const {
	if (row < 0 || row >= row_count()) return "";
	return rows_[static_cast<size_t>(row)].c_str();
}

void RaysWindow::enqueue_request(const RaysRequest &request) {
	requests_.push_back(request);
}

bool RaysWindow::take_request(RaysRequest &request) {
	if (requests_.empty()) return false;
	request = requests_.front();
	requests_.pop_front();
	return true;
}

void RaysWindow::draw(ImGuiPass &pass, uint64_t frame_index) {
	(void)pass;
	(void)frame_index;
	if (!snapshot_.valid) {
		ImGui::TextUnformatted("No ray capture pushed (load a mission).");
		return;
	}

	ImGui::TextUnformatted(snapshot_.recording ? "(recording)" : "(idle)");
	ImGui::SameLine();
	if (ImGui::Button("Clear")) {
		enqueue_request({RaysRequest::Kind::Clear, 0});
	}
	ImGui::Text("logic tick %llu (%.2f s readings)",
			static_cast<unsigned long long>(snapshot_.logic_tick), kRefreshSeconds);

	int ttl_display = ttl_edit_;
	ImGui::SetNextItemWidth(160.0f);
	if (ImGui::SliderInt("Fade ticks", &ttl_display, 15, 620,
				"%d ticks", ImGuiSliderFlags_AlwaysClamp)) {
		ttl_edit_ = ttl_display;
		enqueue_request({RaysRequest::Kind::SetTtlTicks, ttl_edit_});
	}
	ImGui::SameLine();
	ImGui::Text("(%.2f s)", static_cast<float>(ttl_edit_) / static_cast<float>(io::kTicksPerSecondInt));

	if (ImGui::SmallButton("All")) {
		mask_edit_ = kRayCategoryMaskAll;
		enqueue_request({RaysRequest::Kind::SetCategoryMask,
				static_cast<int32_t>(mask_edit_)});
	}
	ImGui::SameLine();
	if (ImGui::SmallButton("None")) {
		mask_edit_ = 0;
		enqueue_request({RaysRequest::Kind::SetCategoryMask, 0});
	}

	if (ImGui::BeginTable("ray_categories", 2,
				ImGuiTableFlags_SizingStretchProp)) {
		const ImVec2 swatch_size(ImGui::GetFrameHeight(), ImGui::GetFrameHeight());
		for (int i = 0; i < kRayCategoryCount; ++i) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::PushID(i);
			bool on = (mask_edit_ & (1u << i)) != 0;
			if (ImGui::Checkbox("##draw", &on)) {
				mask_edit_ = on ? (mask_edit_ | (1u << i))
								: (mask_edit_ & ~(1u << i));
				enqueue_request({RaysRequest::Kind::SetCategoryMask,
						static_cast<int32_t>(mask_edit_)});
			}
			ImGui::SameLine();
			ImGui::ColorButton("##swatch",
					ImVec4(kCategoryColors[i][0], kCategoryColors[i][1],
							kCategoryColors[i][2], 1.0f),
					ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker,
					swatch_size);
			ImGui::SameLine();
			ImGui::TextUnformatted(snapshot_.categories[static_cast<size_t>(i)].name);
			ImGui::TableNextColumn();
			ImGui::Text("held %d / total %llu",
					snapshot_.categories[static_cast<size_t>(i)].held,
					static_cast<unsigned long long>(
							snapshot_.categories[static_cast<size_t>(i)].total));
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
}

}  // namespace opennova::devtools
