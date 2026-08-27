// FrameStatsBoard: the fixed-slot accumulator behind the dev tools' Stats
// window. These cases pin its semantic interface — explicit capture edges,
// atomic drains, and true per-render-frame peaks when a fixed-tick producer
// writes several times — ported from the retired GUT frame_stats_board_test.
#include <runtime/devtools/frame_stats_board.h>

#include <cstdio>
#include <cstring>

using opennova::devtools::CaptureWindow;
using opennova::devtools::FrameStatsBoard;
using opennova::devtools::kSlotCount;
using opennova::devtools::Slot;

namespace {

int g_failures = 0;

#define CHECK(cond, message)                                                          \
	do {                                                                              \
		if (!(cond)) {                                                                \
			std::printf("FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, message, #cond); \
			++g_failures;                                                             \
		}                                                                             \
	} while (0)

int64_t sum_of(const CaptureWindow &w, Slot s) { return w.sums[static_cast<size_t>(s)]; }
int64_t peak_of(const CaptureWindow &w, Slot s) { return w.peaks[static_cast<size_t>(s)]; }
int32_t samples_of(const CaptureWindow &w, Slot s) { return w.sample_frames[static_cast<size_t>(s)]; }

void test_capture_window_accumulates_sums_peaks_and_sample_frames() {
	FrameStatsBoard board;
	CHECK(!board.is_capture_active(), "a new board is not capturing");
	CHECK(board.set_capture_active(true, 10), "the first activation is an edge");
	CHECK(!board.set_capture_active(true, 10), "re-activating is not an edge");
	board.add(Slot::SIM_STEP, 1000, 10);
	board.add(Slot::SIM_STEP, 3000, 10);
	board.add(Slot::OCCL_APPLY, 250, 10);

	const CaptureWindow window = board.drain(10);
	CHECK(sum_of(window, Slot::SIM_STEP) == 4000, "sums accumulate per slot");
	CHECK(peak_of(window, Slot::SIM_STEP) == 4000, "writes in one render frame form one peak");
	CHECK(samples_of(window, Slot::SIM_STEP) == 1, "sample count records distinct render frames, not add calls");
	CHECK(sum_of(window, Slot::OCCL_APPLY) == 250, "the other slot keeps its own sum");
	CHECK(samples_of(window, Slot::PRESENT_FIRE) == 0, "untouched slots stay absent");
}

void test_peak_compares_complete_render_frames() {
	FrameStatsBoard board;
	board.set_capture_active(true, 0);
	board.add(Slot::EFFECTS_TICK, 100, 1);
	board.add(Slot::EFFECTS_TICK, 200, 1);
	board.add(Slot::EFFECTS_TICK, 250, 2);
	const CaptureWindow window = board.drain(3);
	CHECK(sum_of(window, Slot::EFFECTS_TICK) == 550, "the window sums every frame");
	CHECK(peak_of(window, Slot::EFFECTS_TICK) == 300, "the catch-up frame's two fixed ticks beat the later single tick");
	CHECK(samples_of(window, Slot::EFFECTS_TICK) == 2, "two render frames sampled");
}

void test_closed_capture_rejects_feeds_and_edges_reset_the_window() {
	FrameStatsBoard board;
	board.add(Slot::FRAME_HUD, 777, 1);
	board.set_capture_active(true, 1);
	board.add(Slot::FRAME_HUD, 111, 1);
	board.set_capture_active(false, 2);
	board.add(Slot::FRAME_HUD, 999, 2);
	board.set_capture_active(true, 3);
	const CaptureWindow window = board.drain(3);
	CHECK(sum_of(window, Slot::FRAME_HUD) == 0, "capture edges start a clean window and closed feeds are ignored");
	CHECK(peak_of(window, Slot::FRAME_HUD) == 0, "peaks reset on the edge");
	CHECK(samples_of(window, Slot::FRAME_HUD) == 0, "sample counts reset on the edge");
}

void test_drain_reports_render_frames_and_resets_atomically() {
	FrameStatsBoard board;
	board.set_capture_active(true, 100);
	board.add(Slot::FRAME_WALL, 1000, 100);
	const CaptureWindow first = board.drain(102);
	CHECK(first.frames == 2, "window length comes from the render-frame indices, owner-independent");
	CHECK(sum_of(first, Slot::FRAME_WALL) == 1000, "the drained window carries the feed");
	const CaptureWindow second = board.drain(102);
	CHECK(second.frames == 0, "a second drain on the same frame spans no frames");
	CHECK(sum_of(second, Slot::FRAME_WALL) == 0, "drain copies and resets as one operation");
}

void test_slot_names_and_descriptions_cover_the_table() {
	CHECK(kSlotCount > 150, "the slot table survived the transcription");
	CHECK(std::strcmp(opennova::devtools::slot_name(Slot::FRAME_WALL), "FRAME_WALL") == 0, "first slot name");
	CHECK(std::strcmp(opennova::devtools::slot_name(Slot::RENDER_SLOT_VIEWPORTS), "RENDER_SLOT_VIEWPORTS") == 0, "last slot name");
	CHECK(opennova::devtools::slot_name(Slot::COUNT) == nullptr, "COUNT is not a slot");
	CHECK(std::strlen(opennova::devtools::slot_description(Slot::FRAME_WALL)) > 0, "descriptions carried over");
	for (int i = 0; i < kSlotCount; ++i) {
		const char *name = opennova::devtools::slot_name(static_cast<Slot>(i));
		CHECK(name != nullptr && std::strlen(name) > 0, "every slot has a name");
	}
	CHECK(std::strlen(opennova::devtools::slot_description(Slot::COUNT)) == 0, "no description past the table");
}

void test_out_of_range_slots_are_ignored() {
	FrameStatsBoard board;
	board.set_capture_active(true, 0);
	board.add(static_cast<Slot>(-1), 5, 0);
	board.add(Slot::COUNT, 5, 0);
	const CaptureWindow window = board.drain(1);
	for (int i = 0; i < kSlotCount; ++i) {
		CHECK(window.sums[static_cast<size_t>(i)] == 0, "no slot received the stray feeds");
	}
}

}  // namespace

int main() {
	test_capture_window_accumulates_sums_peaks_and_sample_frames();
	test_peak_compares_complete_render_frames();
	test_closed_capture_rejects_feeds_and_edges_reset_the_window();
	test_drain_reports_render_frames_and_resets_atomically();
	test_slot_names_and_descriptions_cover_the_table();
	test_out_of_range_slots_are_ignored();
	if (g_failures != 0) {
		std::printf("%d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("frame_stats_board_test: OK (%d slots)\n", kSlotCount);
	return 0;
}
