// The "Recent Messages" (J-key) window's layout policy.
// [orig: HUD_DrawMessageLog @0x5B9D70; Viewport_ScreenToVirtual @0x5D2C70]

#include <runtime/hud/hud_message_log.h>

#include <cstdio>

using namespace opennova::hud;

namespace {

int failures = 0;
#define CHECK(c, m)                                                            \
	do {                                                                       \
		if (!(c)) { std::fprintf(stderr, "FAIL: %s\n", m); ++failures; }       \
	} while (0)

// The geometry is derived from the SURFACE WIDTH, so these are pinned at two
// widths — a value that only happens to be right at 1024 would pass one and
// fail the other.
void test_geometry_scales_with_width() {
	// At 1024: step = 1024*12/640 = 19 (not 19.2 — integer divide).
	CHECK(message_log_step_px(1024) == 19, "step at 1024 is 19");
	CHECK(message_log_top_px(1024) == 120, "top at 1024 is 120");
	CHECK(message_log_text_top_px(1024) == 130, "text top at 1024 is 130");
	CHECK(message_log_chat_x_px(1024) == 34, "chat x at 1024 is 32 + 2");
	CHECK(message_log_system_x_px(1024) == 990, "system x at 1024 is 990");

	// At 640 the step is exactly 12, which is the constant the divisor is
	// written against — good evidence the /640 is real and not a typo for /1024.
	CHECK(message_log_step_px(640) == 12, "step at 640 is exactly 12");

	// THE ASYMMETRY: the step divides by 640 while the columns shift by 10
	// (/1024). If the step used /1024 too it would be 12 at 1024, not 19.
	CHECK(message_log_step_px(1024) != 12,
			"the step does NOT share the columns' 1024 denominator");

	// At 1920 everything scales, and the +2 on the chat column survives.
	CHECK(message_log_step_px(1920) == 36, "step at 1920");
	CHECK(message_log_top_px(1920) == 225, "top at 1920");
	CHECK(message_log_chat_x_px(1920) == 62, "chat x at 1920 keeps its +2");
}

// The inverse transform rounds via the half-dimension term.
void test_design_space_inverse() {
	// An identity surface maps through unchanged (within the rounding term).
	CHECK(message_log_to_design_x(512, 1024) > 511.0f &&
					message_log_to_design_x(512, 1024) < 513.0f,
			"512 of 1024 maps to ~512 in design x");
	CHECK(message_log_to_design_y(384, 768) > 383.0f &&
					message_log_to_design_y(384, 768) < 385.0f,
			"384 of 768 maps to ~384 in design y");
	// A wider surface compresses screen px into fewer design px.
	CHECK(message_log_to_design_x(960, 1920) > 511.0f &&
					message_log_to_design_x(960, 1920) < 513.0f,
			"half of a 1920 surface is still mid-screen in design space");
	// A zero surface returns the input rather than dividing by zero.
	CHECK(message_log_to_design_x(100, 0) == 100.0f, "zero width is inert");
	CHECK(message_log_to_design_y(100, 0) == 100.0f, "zero height is inert");
}

// THE ROW ORDER. The ring walk paints row 15 first and steps down to row 0, and
// the sinks put the NEWEST line in row 0 — so the oldest shown line is the TOP
// row, and a short history leaves the TOP rows blank rather than bottom-filling.
void test_row_order_and_padding() {
	// A full history: row 0 shows the oldest of the 16 shown.
	CHECK(message_log_row_source(0, 16) == 0, "full history starts at index 0");
	CHECK(message_log_row_source(15, 16) == 15, "and ends at the newest");

	// More history than rows: only the NEWEST 16 are shown, still oldest-first.
	CHECK(message_log_row_source(0, 20) == 4,
			"a 20-line history shows lines 4..19, oldest at the top");
	CHECK(message_log_row_source(15, 20) == 19, "and the newest at the bottom");

	// A SHORT history pads at the TOP. With 3 lines, rows 0..12 are blank and
	// the lines occupy the bottom three rows, oldest first.
	CHECK(message_log_row_source(0, 3) == -1, "short history leaves row 0 blank");
	CHECK(message_log_row_source(12, 3) == -1, "blank right up to the pad edge");
	CHECK(message_log_row_source(13, 3) == 0, "the oldest line starts the run");
	CHECK(message_log_row_source(14, 3) == 1, "then the next");
	CHECK(message_log_row_source(15, 3) == 2, "and the newest sits last");

	// An empty history draws nothing at all.
	for (int r = 0; r < kMessageLogRows; ++r)
		CHECK(message_log_row_source(r, 0) == -1, "an empty ring draws no rows");

	// Out-of-range rows are rejected rather than clamped.
	CHECK(message_log_row_source(-1, 16) == -1, "negative row rejected");
	CHECK(message_log_row_source(16, 16) == -1, "past the last row rejected");
}

} // namespace

int main() {
	test_geometry_scales_with_width();
	test_design_space_inverse();
	test_row_order_and_padding();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("hud_message_log_test OK\n");
	return 0;
}
