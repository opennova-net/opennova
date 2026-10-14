#include <runtime/devtools/log_window.h>

#include <base/io/log_ring.h>
#include <base/io/strutil.h>
#include <runtime/devtools/ui_kit.h>

#include <imgui.h>

#include <cstdio>
#include <string_view>
#include <utility>

namespace opennova::devtools {

namespace {

const char *const kLevelNames[LogWindow::kLevelCount] = {"debug", "info", "warn", "error", "cmd"};

ImVec4 level_color(LogWindow::Level level) {
	switch (level) {
		case LogWindow::kDebug:
			return ImVec4(0.6f, 0.6f, 0.65f, 1.0f);
		case LogWindow::kWarn:
			return ImVec4(1.0f, 0.82f, 0.35f, 1.0f);
		case LogWindow::kError:
			return ImVec4(1.0f, 0.42f, 0.35f, 1.0f);
		case LogWindow::kCommand:
			return ImVec4(0.45f, 0.85f, 1.0f, 1.0f);
		default:
			return ImGui::GetStyleColorVec4(ImGuiCol_Text);
	}
}

}  // namespace

void LogWindow::on_visibility(bool visible) {
	if (visible) {
		scroll_to_bottom_ = true;
	}
}

void LogWindow::push_row(Row row) {
	rows_.push_back(std::move(row));
	while (rows_.size() > kMaxRows) {
		rows_.pop_front();
	}
	filter_dirty_ = true;
	if (auto_scroll_) scroll_to_bottom_ = true;
}

void LogWindow::poll() {
	if (ring_ == nullptr) return;
	const std::vector<io::LogRingEntry> entries = ring_->entries_after(cursor_);
	if (entries.empty()) return;
	if (cursor_ != 0 && entries.front().sequence > cursor_ + 1) {
		Row gap;
		gap.level = kWarn;
		char buf[96];
		std::snprintf(buf, sizeof(buf), "[log] the ring wrapped: %llu message(s) missed",
				static_cast<unsigned long long>(entries.front().sequence - cursor_ - 1));
		gap.text = buf;
		push_row(std::move(gap));
	}
	for (const io::LogRingEntry &entry : entries) {
		Row row;
		row.sequence = entry.sequence;
		row.level = static_cast<Level>(static_cast<int>(entry.level));
		row.text = std::string("[") + io::log_level_name(entry.level) + "] " + entry.text;
		push_row(std::move(row));
	}
	cursor_ = entries.back().sequence;
}

void LogWindow::add_command_result(const ControlResult &result) {
	Row row;
	row.level = kCommand;
	row.text = "[cmd] " + result.id + (result.ok ? ": ok" : ": failed");
	if (!result.message.empty()) row.text += " (" + result.message + ")";
	if (!result.detail.empty()) row.text += " = " + result.detail;
	push_row(std::move(row));
}

void LogWindow::clear() {
	rows_.clear();
	filter_dirty_ = true;
}

void LogWindow::set_level_mask(uint32_t mask) {
	level_mask_ = mask;
	filter_dirty_ = true;
}

void LogWindow::set_text_filter(const std::string &filter) {
	text_filter_ = filter;
	std::snprintf(filter_edit_, sizeof(filter_edit_), "%s", filter.c_str());
	filter_dirty_ = true;
}

bool LogWindow::passes(const Row &row) const {
	return (level_mask_ & (1u << row.level)) != 0 &&
			strutil::ifind(row.text, text_filter_) != std::string_view::npos;
}

void LogWindow::refilter() {
	if (!filter_dirty_) return;
	filter_dirty_ = false;
	filtered_.clear();
	for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
		if (passes(rows_[static_cast<size_t>(i)])) filtered_.push_back(i);
	}
}

int LogWindow::row_count() const {
	const_cast<LogWindow *>(this)->refilter();
	return static_cast<int>(filtered_.size());
}

const char *LogWindow::row_text(int row) const {
	if (row < 0 || row >= row_count()) return "";
	return rows_[static_cast<size_t>(filtered_[static_cast<size_t>(row)])].text.c_str();
}

void LogWindow::draw(ImGuiPass &pass, uint64_t frame_index) {
	(void)pass;
	(void)frame_index;
	poll();
	ui_kit::WrapRow row;
	for (int level = 0; level < kLevelCount; ++level) {
		bool on = (level_mask_ & (1u << level)) != 0;
		ImGui::PushStyleColor(ImGuiCol_Text, level_color(static_cast<Level>(level)));
		const bool changed = ui_kit::check(row, kLevelNames[level], &on, "");
		ImGui::PopStyleColor();
		if (changed) set_level_mask(on ? (level_mask_ | (1u << level)) : (level_mask_ & ~(1u << level)));
	}
	constexpr float kFilterWidth = 200.0f;
	row.next(kFilterWidth);
	if (ui_kit::filter_box("##filter", filter_edit_, sizeof(filter_edit_), "filter", kFilterWidth)) {
		text_filter_ = filter_edit_;
		filter_dirty_ = true;
	}
	ui_kit::check(row, "Auto-scroll", &auto_scroll_, "Follow the newest row.");
	if (ui_kit::tool(row, "Clear", !rows_.empty(), rows_.empty() ? "Nothing to clear." : "Empty the log.")) {
		clear();
	}
	const bool copy = ui_kit::tool(row, "Copy", !filtered_.empty(),
			filtered_.empty() ? "Nothing to copy." : "Copy the rows the filters show to the clipboard.");
	refilter();
	char count[48];
	std::snprintf(count, sizeof(count), "%d / %d rows", static_cast<int>(filtered_.size()), total_rows());
	row.next(ui_kit::text_width(count));
	ImGui::TextDisabled("%s", count);
	ImGui::Separator();

	if (ImGui::BeginChild("log_rows", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
				ImGuiWindowFlags_HorizontalScrollbar)) {
		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(filtered_.size()));
		while (clipper.Step()) {
			for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
				const Row &row = rows_[static_cast<size_t>(filtered_[static_cast<size_t>(i)])];
				ImGui::PushStyleColor(ImGuiCol_Text, level_color(row.level));
				ImGui::TextUnformatted(row.text.c_str());
				ImGui::PopStyleColor();
			}
		}
		clipper.End();
		if (copy) {
			// The clipper draws only the visible rows; the copy takes them all.
			std::string all;
			for (const int index : filtered_) {
				all += rows_[static_cast<size_t>(index)].text;
				all += '\n';
			}
			ImGui::SetClipboardText(all.c_str());
		}
		if (scroll_to_bottom_ || (auto_scroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())) {
			ImGui::SetScrollHereY(1.0f);
		}
		scroll_to_bottom_ = false;
	}
	ImGui::EndChild();
}

}  // namespace opennova::devtools
