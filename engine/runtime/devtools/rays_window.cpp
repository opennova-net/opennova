#include <runtime/devtools/rays_window.h>
#include <base/io/tick_rate.h>
#include <runtime/devtools/capture_table.h>
#include <runtime/devtools/ui_kit.h>

#include <imgui.h>

namespace opennova::devtools {

void RaysWindow::on_visibility(bool visible) {
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
		rows_[static_cast<size_t>(i)] = capture_row_text(snapshot_.categories[static_cast<size_t>(i)]);
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
	(void)frame_index;
	ui_kit::WrapRow layers;
	bool layer_on = overlay_layer_.enabled();
	if (ui_kit::check(layers, "Show in Game view", &layer_on, overlay_layer_.tooltip())) {
		pass.set_overlay_enabled(overlay_layer_, layer_on);
	}
	if (!snapshot_.valid) {
		ui_kit::empty_state("No ray capture pushed (load a mission).");
		return;
	}

	ui_kit::WrapRow capture;
	const char *state = snapshot_.recording ? "(recording)" : "(idle)";
	capture.next(ui_kit::text_width(state));
	ImGui::TextUnformatted(state);
	if (ui_kit::tool(capture, "Clear", true, "Drop the captured rays.")) {
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
	ui_kit::tooltip("How long a ray stays in the Game view, fading out.");
	ImGui::SameLine();
	ImGui::Text("(%.2f s)", static_cast<float>(ttl_edit_) / static_cast<float>(io::kTicksPerSecondInt));

	if (capture_mask_table("ray_categories", mask_edit_, kRayCategoryMaskAll, snapshot_.categories,
				kRayCategoryColors, kRayCategoryCount)) {
		enqueue_request({RaysRequest::Kind::SetCategoryMask, static_cast<int32_t>(mask_edit_)});
	}
}

}  // namespace opennova::devtools
