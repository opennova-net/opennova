#include <runtime/devtools/stats_window.h>

#include <runtime/devtools/stats_window_rows.h>

#include <imgui.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace opennova::devtools {

namespace {

using stats_rows::kRowCount;
using stats_rows::kRows;

int64_t slot_sum(const CaptureWindow &w, Slot slot) {
	return w.sums[static_cast<size_t>(slot)];
}

int64_t slot_peak(const CaptureWindow &w, Slot slot) {
	return w.peaks[static_cast<size_t>(slot)];
}

int32_t slot_samples(const CaptureWindow &w, Slot slot) {
	return w.sample_frames[static_cast<size_t>(slot)];
}

// Window mean per render frame, in milliseconds.
double mean_ms(const CaptureWindow &w, int64_t sum_us) {
	return w.frames == 0 ? 0.0 : static_cast<double>(sum_us) / 1000.0 / static_cast<double>(w.frames);
}

double mean_count(const CaptureWindow &w, Slot slot) {
	return w.frames == 0 ? 0.0 : static_cast<double>(slot_sum(w, slot)) / static_cast<double>(w.frames);
}

template <size_t N>
void set_text(std::array<char, N> &dst, const char *text) {
	std::snprintf(dst.data(), N, "%s", text);
}

template <size_t N>
void set_ms(std::array<char, N> &dst, double ms) {
	std::snprintf(dst.data(), N, "%.2f", ms);
}

}  // namespace

StatsWindow::StatsWindow() : texts_(static_cast<size_t>(kRowCount)) {
	format_rows();
}

void StatsWindow::set_board(FrameStatsBoard *board) {
	if (board_ == board) {
		return;
	}
	if (board_ != nullptr) {
		board_->set_capture_active(false, last_frame_index_);
	}
	board_ = board;
	if (board_ != nullptr) {
		board_->set_capture_active(shown_ && !external_, last_frame_index_);
	}
}

void StatsWindow::on_visibility(bool visible) {
	shown_ = visible;
	if (!visible) {
		// An external feed lasts as long as the feeder's session: the next
		// opening drains the board again.
		external_ = false;
		last_refresh_time_ = -1.0;
	}
	if (board_ != nullptr) {
		board_->set_capture_active(visible && !external_, last_frame_index_);
	}
}

void StatsWindow::feed_external(const CaptureWindow &window) {
	external_ = true;
	if (board_ != nullptr) {
		board_->set_capture_active(false, last_frame_index_);
	}
	apply_reading(window);
}

void StatsWindow::apply_reading(const CaptureWindow &window) {
	reading_ = window;
	if (window.frames > 0 && slot_samples(window, Slot::FRAME_WALL) > 0) {
		frame_history_[static_cast<size_t>(frame_history_next_)] =
				static_cast<float>(mean_ms(window, slot_sum(window, Slot::FRAME_WALL)));
		frame_history_next_ = (frame_history_next_ + 1) % static_cast<int>(frame_history_.size());
		if (frame_history_count_ < static_cast<int>(frame_history_.size())) {
			++frame_history_count_;
		}
	}
	format_rows();
	format_info();
}

void StatsWindow::format_rows() {
	const CaptureWindow &w = reading_;
	for (int i = 0; i < kRowCount; ++i) {
		const StatsRow &row = kRows[i];
		RowText &text = texts_[static_cast<size_t>(i)];
		text.average[0] = '\0';
		text.peak[0] = '\0';
		if (w.frames == 0) {
			continue;
		}
		switch (row.kind) {
			case RowKind::SPAN:
				if (slot_samples(w, row.slot) > 0) {
					set_ms(text.average, mean_ms(w, slot_sum(w, row.slot)));
					set_ms(text.peak, static_cast<double>(slot_peak(w, row.slot)) / 1000.0);
				}
				break;
			case RowKind::GROUP: {
				int64_t sum = 0;
				bool any = false;
				for (int k = 0; k < row.slot_count; ++k) {
					sum += slot_sum(w, row.slots[k]);
					any = any || slot_samples(w, row.slots[k]) > 0;
				}
				if (any) {
					set_ms(text.average, mean_ms(w, sum));
				}
				break;
			}
			case RowKind::RESIDUAL: {
				if (slot_samples(w, row.slot) > 0) {
					int64_t sum = slot_sum(w, row.slot);
					for (int k = 0; k < row.slot_count; ++k) {
						sum -= slot_sum(w, row.slots[k]);
					}
					set_ms(text.average, mean_ms(w, sum));
				}
				break;
			}
			case RowKind::HEADER:
				break;
		}
	}
}

void StatsWindow::set_info(const char *row_id, const char *text) {
	for (int i = 0; i < kRowCount; ++i) {
		if (std::strcmp(kRows[i].id, row_id) == 0) {
			set_text(texts_[static_cast<size_t>(i)].info, text);
			return;
		}
	}
}

// The counter cells backed by the board's VALUE slots. The retired page also
// pulled live counters off Godot-side objects (Performance monitors, the
// occlusion/effect/present stats); those return as VALUE slots the shell
// samplers feed (TODO.md, "Stats window info cells").
void StatsWindow::format_info() {
	const CaptureWindow &w = reading_;
	for (auto &text : texts_) {
		text.info[0] = '\0';
	}
	if (w.frames == 0) {
		return;
	}
	char buf[96];

	// Frame row: fps from the wall clock, plus Godot's own once-per-second
	// worst-iteration process time as the cross-check for the PEAK column.
	if (slot_samples(w, Slot::FRAME_WALL) > 0) {
		const double wall_ms = mean_ms(w, slot_sum(w, Slot::FRAME_WALL));
		int n = std::snprintf(buf, sizeof(buf), "%d fps", wall_ms > 0.0 ? static_cast<int>(std::lround(1000.0 / wall_ms)) : 0);
		if (slot_samples(w, Slot::FRAME_TIME_PROCESS) > 0 && n > 0) {
			std::snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), " | godot process peak %.2f ms",
					static_cast<double>(slot_peak(w, Slot::FRAME_TIME_PROCESS)) / 1000.0);
		}
		set_info("frame", buf);
	}
	if (slot_samples(w, Slot::FRAME_PHYSICS_ITERATIONS) > 0) {
		std::snprintf(buf, sizeof(buf), "server peak %.2f ms | %.1f iter/f",
				static_cast<double>(slot_peak(w, Slot::FRAME_PHYSICS_SERVER)) / 1000.0,
				mean_count(w, Slot::FRAME_PHYSICS_ITERATIONS));
		set_info("physics_callbacks", buf);
	}
	if (slot_samples(w, Slot::FRAME_NODES_FREED) > 0) {
		std::snprintf(buf, sizeof(buf), "%.1f freed | %.1f added nodes/f",
				mean_count(w, Slot::FRAME_NODES_FREED), mean_count(w, Slot::FRAME_NODES_ADDED));
		set_info("flush_tail", buf);
	}
	struct PassCounts {
		const char *row;
		Slot objects;
		Slot draws;
	};
	static constexpr PassCounts kPasses[] = {
		{"render_main", Slot::RENDER_MAIN_OBJECTS, Slot::RENDER_MAIN_DRAWS},
		{"render_shadow", Slot::RENDER_SHADOW_OBJECTS, Slot::RENDER_SHADOW_DRAWS},
		{"render_water", Slot::RENDER_WATER_OBJECTS, Slot::RENDER_WATER_DRAWS},
		{"render_q3", Slot::RENDER_Q3_OBJECTS, Slot::RENDER_Q3_DRAWS},
	};
	for (const PassCounts &pass : kPasses) {
		// A pass that never sampled (no water, capture just opened) keeps its
		// cleared cell.
		if (slot_samples(w, pass.objects) > 0) {
			std::snprintf(buf, sizeof(buf), "%d objs | %d draws",
					static_cast<int>(mean_count(w, pass.objects)), static_cast<int>(mean_count(w, pass.draws)));
			set_info(pass.row, buf);
		}
	}
	if (slot_samples(w, Slot::RENDER_SLOT_CAPTURES) > 0) {
		std::snprintf(buf, sizeof(buf), "%d objs | %d draws | %.1f captures/f | %d verts | %d skinned",
				static_cast<int>(mean_count(w, Slot::RENDER_SLOT_OBJECTS)),
				static_cast<int>(mean_count(w, Slot::RENDER_SLOT_DRAWS)),
				mean_count(w, Slot::RENDER_SLOT_CAPTURES),
				static_cast<int>(mean_count(w, Slot::RENDER_SLOT_PACKED_VERTICES)),
				static_cast<int>(mean_count(w, Slot::RENDER_SLOT_SKINNED)));
		set_info("render_slot", buf);
	}
	if (slot_samples(w, Slot::MODEL_AWAKE_MODELS) > 0) {
		const double samples = static_cast<double>(slot_samples(w, Slot::MODEL_AWAKE_MODELS));
		std::snprintf(buf, sizeof(buf), "%d awake | %d renderable",
				static_cast<int>(std::lround(static_cast<double>(slot_sum(w, Slot::MODEL_AWAKE_MODELS)) / samples)),
				static_cast<int>(std::lround(static_cast<double>(slot_sum(w, Slot::MODEL_RENDERABLE_MODELS)) / samples)));
		set_info("material", buf);
	}
	if (slot_samples(w, Slot::PRESENT_MISSION_ROWS) > 0) {
		std::snprintf(buf, sizeof(buf), "%d rows | %d submitted | %d body",
				static_cast<int>(std::lround(mean_count(w, Slot::PRESENT_MISSION_ROWS))),
				static_cast<int>(std::lround(mean_count(w, Slot::PRESENT_MISSION_SUBMITTED_ROWS))),
				static_cast<int>(std::lround(mean_count(w, Slot::PRESENT_MISSION_BODY_ROWS))));
		set_info("mission_rows", buf);
	}
	if (slot_samples(w, Slot::PRESENT_WIRE_LIVE) > 0) {
		std::snprintf(buf, sizeof(buf), "%d nodes | %d pending",
				static_cast<int>(std::lround(mean_count(w, Slot::PRESENT_WIRE_LIVE))),
				static_cast<int>(std::lround(mean_count(w, Slot::PRESENT_WIRE_PENDING))));
		set_info("wire_rows", buf);
	}
	if (slot_samples(w, Slot::SIM_TICKS) > 0) {
		int n = std::snprintf(buf, sizeof(buf), "%.1f t/f", mean_count(w, Slot::SIM_TICKS));
		if (n > 0 && slot_samples(w, Slot::SIM_ENTITY_COUNT) > 0) {
			// The role is constant over a capture, so its mean is its value.
			static const char *const kRoleNames[] = {"single player", "host", "joiner"};
			const int role = static_cast<int>(std::lround(mean_count(w, Slot::SIM_ROLE)));
			const char *role_name = (role >= 0 && role < 3) ? kRoleNames[role] : "?";
			std::snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), " | %d entities | %s",
					static_cast<int>(std::lround(mean_count(w, Slot::SIM_ENTITY_COUNT))),
					role_name);
		}
		set_info("sim", buf);
	}
	if (slot_samples(w, Slot::NET_PEER_COUNT) > 0) {
		std::snprintf(buf, sizeof(buf), "%d peers",
				static_cast<int>(std::lround(mean_count(w, Slot::NET_PEER_COUNT))));
		set_info("net", buf);
	}
	if (slot_samples(w, Slot::TRACE_CALLS) > 0) {
		std::snprintf(buf, sizeof(buf), "%.1f calls/f | S %.1f D %.1f P %.1f surv/f | %.1f/%.1f faces/f",
				mean_count(w, Slot::TRACE_CALLS), mean_count(w, Slot::TRACE_STATIC_SURVIVORS),
				mean_count(w, Slot::TRACE_DYNAMIC_SURVIVORS), mean_count(w, Slot::TRACE_PERSON_SURVIVORS),
				mean_count(w, Slot::TRACE_STATIC_FACES), mean_count(w, Slot::TRACE_DYNAMIC_FACES));
		set_info("trace", buf);
	}
	if (slot_samples(w, Slot::EFFECTS_DRAIN) > 0) {
		std::snprintf(buf, sizeof(buf), "drain %.2f ms/f", mean_ms(w, slot_sum(w, Slot::EFFECTS_DRAIN)));
		set_info("effects", buf);
	}
}

int StatsWindow::row_count() const {
	return kRowCount;
}

const char *StatsWindow::row_id(int row) const {
	return (row >= 0 && row < kRowCount) ? kRows[row].id : "";
}

const char *StatsWindow::row_average(int row) const {
	return (row >= 0 && row < kRowCount) ? texts_[static_cast<size_t>(row)].average.data() : "";
}

const char *StatsWindow::row_peak(int row) const {
	return (row >= 0 && row < kRowCount) ? texts_[static_cast<size_t>(row)].peak.data() : "";
}

const char *StatsWindow::row_info(int row) const {
	return (row >= 0 && row < kRowCount) ? texts_[static_cast<size_t>(row)].info.data() : "";
}

// Emit the row at `index` and, when it is an open tree node, its descendants
// (the rows that follow with a greater depth). Returns the index of the next
// row at `depth` or shallower.
int StatsWindow::draw_rows(int index, int depth) {
	while (index < kRowCount && kRows[index].depth >= depth) {
		const StatsRow &row = kRows[index];
		if (row.depth > depth) {
			// A deeper row under a collapsed parent: skip the subtree.
			++index;
			continue;
		}
		const bool has_children = index + 1 < kRowCount && kRows[index + 1].depth > row.depth;
		const RowText &text = texts_[static_cast<size_t>(index)];

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_DefaultOpen;
		if (!has_children) {
			flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_Bullet;
		}
		const bool node_open = ImGui::TreeNodeEx(row.id, flags, "%s", row.label);
		ImGui::TableNextColumn();
		ImGui::TextUnformatted(text.average.data());
		ImGui::TableNextColumn();
		ImGui::TextUnformatted(text.peak.data());
		ImGui::TableNextColumn();
		ImGui::TextUnformatted(text.info.data());

		++index;
		if (has_children) {
			if (node_open) {
				index = draw_rows(index, row.depth + 1);
				ImGui::TreePop();
			} else {
				while (index < kRowCount && kRows[index].depth > row.depth) {
					++index;
				}
			}
		}
	}
	return index;
}

void StatsWindow::draw(ImGuiPass &pass, uint64_t frame_index) {
	(void)pass;
	last_frame_index_ = frame_index;

	if (!external_ && board_ != nullptr) {
		const double now = ImGui::GetTime();
		if (last_refresh_time_ < 0.0 || now - last_refresh_time_ >= kRefreshSeconds) {
			last_refresh_time_ = now;
			apply_reading(board_->drain(frame_index));
		}
	}

	if (board_ == nullptr && !external_) {
		ImGui::TextUnformatted("No frame-stats board bound.");
		return;
	}
	if (reading_.frames > 0) {
		ImGui::Text("Window: %llu render frames (%.1f s readings); the dev tools' own cost is included.",
				static_cast<unsigned long long>(reading_.frames), kRefreshSeconds);
	} else {
		ImGui::TextUnformatted("Capturing...");
	}
	if (frame_history_count_ > 0) {
		float peak = 0.0f;
		for (int i = 0; i < frame_history_count_; ++i) {
			peak = frame_history_[static_cast<size_t>(i)] > peak ? frame_history_[static_cast<size_t>(i)] : peak;
		}
		char overlay[48];
		std::snprintf(overlay, sizeof(overlay), "frame wall ms (peak %.1f)", peak);
		ImGui::PlotLines("##frame_wall", frame_history_.data(), frame_history_count_, frame_history_next_,
				overlay, 0.0f, peak * 1.1f + 0.1f, ImVec2(-1.0f, 48.0f));
	}

	const ImGuiTableFlags table_flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
			ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
	if (ImGui::BeginTable("stats_rows", 4, table_flags)) {
		ImGui::TableSetupScrollFreeze(0, 1);
		// The tree runs ten levels deep: the name column takes most of the width.
		ImGui::TableSetupColumn("System", ImGuiTableColumnFlags_WidthStretch, 5.0f);
		ImGui::TableSetupColumn("Avg ms", ImGuiTableColumnFlags_WidthFixed, 64.0f);
		ImGui::TableSetupColumn("Peak ms", ImGuiTableColumnFlags_WidthFixed, 64.0f);
		ImGui::TableSetupColumn("Info", ImGuiTableColumnFlags_WidthStretch, 2.0f);
		ImGui::TableHeadersRow();
		ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, ImGui::GetFontSize() * 0.75f);
		draw_rows(0, 0);
		ImGui::PopStyleVar();
		ImGui::EndTable();
	}
}

}  // namespace opennova::devtools
