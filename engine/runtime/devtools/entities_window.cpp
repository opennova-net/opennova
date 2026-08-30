#include <runtime/devtools/entities_window.h>

#include <imgui.h>

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <utility>

namespace opennova::devtools {

namespace {

constexpr uint16_t kNoHandle = world::EntityHandle::kInvalid;

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
		// needs_entity_directory gate stops the pushes on the same edge. The
		// selection survives as a pending handle so reopening re-selects it.
		const uint16_t keep = selected_handle();
		snapshot_ = EntityDirectorySnapshot{};
		format_rows();
		selected_ = -1;
		pending_select_handle_ = keep;
		scroll_to_selected_ = false;
	}
}

void EntitiesWindow::set_directory(EntityDirectorySnapshot snapshot) {
	snapshot_ = std::move(snapshot);
	if (!snapshot_.valid) {
		snapshot_.rows.clear();
	}
	format_rows();
	apply_pending_selection();
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

int EntitiesWindow::row_index_for_handle(uint16_t handle) const {
	if (handle == kNoHandle) {
		return -1;
	}
	for (size_t i = 0; i < snapshot_.rows.size(); ++i) {
		if (snapshot_.rows[i].wire_handle == handle) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

void EntitiesWindow::format_rows() {
	texts_.clear();
	texts_.reserve(snapshot_.rows.size());
	// Keep the selection across pushes by wire handle (the directory reorders
	// as entities die and spawn); a selection the push no longer carries is
	// gone with its entity.
	const uint16_t selected_handle_before = selected_row() != nullptr
			? selected_row()->wire_handle
			: kNoHandle;
	for (size_t i = 0; i < snapshot_.rows.size(); ++i) {
		const world::inspect::EntityRow &row = snapshot_.rows[i];
		RowText text;
		text.name = row.name.empty() ? "(unnamed)" : row.name;
		text.item = row.item_name.empty() ? "-" : row.item_name;
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
	}
	selected_ = row_index_for_handle(selected_handle_before);
	apply_filter();
}

void EntitiesWindow::apply_filter() {
	filtered_.clear();
	for (size_t i = 0; i < snapshot_.rows.size(); ++i) {
		const RowText &text = texts_[i];
		if (contains_ci(text.name.c_str(), filter_.data()) ||
				contains_ci(text.item.c_str(), filter_.data()) ||
				contains_ci(text.net_id.data(), filter_.data())) {
			filtered_.push_back(static_cast<int>(i));
		}
	}
}

void EntitiesWindow::apply_pending_selection() {
	if (pending_select_handle_ == kNoHandle || !snapshot_.valid) {
		return;
	}
	const int index = row_index_for_handle(pending_select_handle_);
	// A push without the handle drops it: the entity is not in the directory.
	pending_select_handle_ = kNoHandle;
	if (index >= 0) {
		// The pick's intent is "show me this": a filter typed meanwhile must
		// not hide the row it lands on.
		set_filter("");
		select_row(index);
		scroll_to_selected_ = true;
	}
}

void EntitiesWindow::select_row(int snapshot_index) {
	selected_ = snapshot_index;
	pending_select_handle_ = kNoHandle;
}

void EntitiesWindow::select_handle(uint16_t handle) {
	if (handle == kNoHandle) {
		clear_selection();
		return;
	}
	open = true;
	request_focus();
	set_filter("");
	const int index = row_index_for_handle(handle);
	if (index >= 0) {
		select_row(index);
		scroll_to_selected_ = true;
		return;
	}
	// Not in the held directory (none pushed yet, or a fresh pick the next
	// push will carry): deselect and wait for the push.
	selected_ = -1;
	pending_select_handle_ = handle;
}

void EntitiesWindow::clear_selection() {
	selected_ = -1;
	pending_select_handle_ = kNoHandle;
	scroll_to_selected_ = false;
}

uint16_t EntitiesWindow::selected_handle() const {
	if (const world::inspect::EntityRow *row = selected_row()) {
		return row->wire_handle;
	}
	return pending_select_handle_;
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

const char *EntitiesWindow::row_item(int row) const {
	return (row >= 0 && row < row_count()) ? texts_[static_cast<size_t>(filtered_[static_cast<size_t>(row)])].item.c_str() : "";
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

void EntitiesWindow::draw(ImGuiPass &pass, uint64_t frame_index) {
	(void)pass;
	(void)frame_index;
	if (!snapshot_.valid) {
		ImGui::TextUnformatted("No entity directory pushed (load a mission).");
		if (pending_select_handle_ != kNoHandle) {
			ImGui::Text("Selected entity %d:%d awaits the next directory push.",
					(pending_select_handle_ >> 12) & 0xF, pending_select_handle_ & 0xFFF);
		}
		return;
	}
	ImGui::Text("%d of %d entities | logic tick %llu (%.1f s readings)",
			row_count(), static_cast<int>(snapshot_.rows.size()),
			static_cast<unsigned long long>(snapshot_.logic_tick), kRefreshSeconds);
	ImGui::SetNextItemWidth(-64.0f);
	if (ImGui::InputText("Filter", filter_.data(), filter_.size())) {
		apply_filter();
	}

	const ImGuiTableFlags table_flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
			ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
	if (ImGui::BeginTable("entity_rows", 8, table_flags, ImVec2(0.0f, 0.0f))) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 3.0f);
		ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch, 2.5f);
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
			if (index == selected_ && scroll_to_selected_) {
				// Consumed only when the row is emitted, so a frame that draws
				// no table (the window collapsed) keeps the request.
				ImGui::SetScrollHereY(0.5f);
				scroll_to_selected_ = false;
			}
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(text.item.c_str());
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
}

}  // namespace opennova::devtools
