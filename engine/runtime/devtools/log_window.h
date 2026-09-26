// The Log window: the engine's io::log channel as a filterable, scrolling
// console, plus every F3 command's verdict. It reads the process LogRing
// (base/io/log_ring.h, the same ring MCP's game_logs "engine" source drains)
// through the pointer the composer lends it — the Stats window's board
// pattern: an engine structure, never Godot — and drains only the entries
// after its cursor, so a frame with nothing new copies nothing. The ring
// holds the newest 512 messages; a drain that finds a gap past its cursor
// marks how many it missed.
//
// Visibility-armed: while hidden it does not drain (the ring keeps
// recording, so reopening catches up from the cursor, up to the ring's
// capacity). Clear drops the displayed rows and keeps the cursor.
#pragma once

#include <runtime/devtools/control_request.h>
#include <runtime/devtools/imgui_pass.h>

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace opennova::io {
class LogRing;
}

namespace opennova::devtools {

class LogWindow : public Window {
public:
	// Row levels: the four io::LogLevel values, then F3 command verdicts.
	enum Level : uint8_t { kDebug = 0, kInfo, kWarn, kError, kCommand, kLevelCount };
	static constexpr size_t kMaxRows = 2000;

	const char *title() const override { return "Log"; }
	MenuGroup menu_group() const override { return MenuGroup::Tools; }
	WindowSizeHint preferred_size() const override { return {900.0f, 320.0f}; }
	void draw(ImGuiPass &pass, uint64_t frame_index) override;
	void on_visibility(bool visible) override;

	// The ring to drain (process-wide, owned elsewhere; may be null).
	void set_ring(const io::LogRing *ring) { ring_ = ring; }
	// Drain the entries past the cursor into the rows (draw does this each
	// frame while shown; the test seam calls it directly).
	void poll();
	// One F3 command verdict as a row (every verdict, shown or not).
	void add_command_result(const ControlResult &result);
	void clear();

	// Filters: bit i shows level i; the text filter is a case-insensitive
	// substring (empty = everything).
	void set_level_mask(uint32_t mask);
	void set_text_filter(const std::string &filter);

	// The rows passing the filters, oldest first ("[level] text"), for tests.
	int row_count() const;
	const char *row_text(int row) const;
	int total_rows() const { return static_cast<int>(rows_.size()); }

private:
	struct Row {
		uint64_t sequence = 0; // 0 for a command verdict or a gap marker
		Level level = kInfo;
		std::string text;      // "[level] message"
	};

	void push_row(Row row);
	void refilter();
	bool passes(const Row &row) const;

	const io::LogRing *ring_ = nullptr;
	uint64_t cursor_ = 0;
	std::deque<Row> rows_;
	std::vector<int> filtered_; // indices into rows_
	bool filter_dirty_ = true;
	uint32_t level_mask_ = (1u << kLevelCount) - 1u;
	std::string text_filter_;
	char filter_edit_[128] = {};
	bool auto_scroll_ = true;
	bool scroll_to_bottom_ = false;
};

}  // namespace opennova::devtools
