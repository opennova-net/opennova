// Pins the witnessed .mnu widget-geometry solves
// [orig: CUIElement_DrawFrame @ 0x64a210; the POSITION parse tail @ 0x648120;
// CStaticWnd_AdjustRectToTextSize @ 0x6575f0; CSpinListWnd_CreateUpDownChildren
// @ 0x64b8b0; the ITEM color parse @ 0x64bd10/@ 0x64b220].

#include <formats/mnu/mnu_layout.h>

#include <cmath>
#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const char *message) {
	if (!ok) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		++failures;
	}
}

bool near(float actual, float expected) {
	return std::fabs(actual - expected) <= 0.0001f;
}

void frame_contract() {
	// The stencil grid slices SIZE x SIZE tiles [orig: CUIElement_InitBorderMaterials
	// @ 0x646f70]; the authored STENCIL attr wins, else width/4.
	const opennova::mnu::FrameTileRect tile = opennova::mnu::frame_tile_rect(16, 2, 1);
	check(tile.x == 32 && tile.y == 16 && tile.size == 16,
			"tile rects slice the SIZE grid");
	check(opennova::mnu::frame_stencil_tile_size(24, 64) == 24,
			"the authored STENCIL size wins");
	check(opennova::mnu::frame_stencil_tile_size(0, 64) == 16,
			"an unauthored size recovers width/4");

	// The 8 border pieces hang OUTSIDE the rect by SIZE, pulled back by the
	// insets; edges stretch between the corners [orig: @ 0x64a210].
	const auto layout = opennova::mnu::frame_border_layout(16, 2, 3);
	const auto &tl = layout[0];
	check(tl.tile_col == 0 && tl.tile_row == 0 &&
					near(tl.anchor_left, 0) && near(tl.anchor_right, 0) &&
					near(tl.off_left, -14.0f) && near(tl.off_top, -13.0f) &&
					near(tl.off_right, 2.0f) && near(tl.off_bottom, 3.0f),
			"TL overhangs by SIZE minus the insets");
	const auto &top = layout[1];
	check(top.tile_col == 1 && near(top.anchor_left, 0) &&
					near(top.anchor_right, 1) && near(top.off_left, 2.0f) &&
					near(top.off_right, -3.0f),
			"the top edge stretches between the corners (trailing -inset-1)");
	const auto &br = layout[7];
	check(br.tile_col == 2 && br.tile_row == 2 &&
					near(br.anchor_left, 1) && near(br.anchor_top, 1) &&
					near(br.off_left, -3.0f) && near(br.off_top, -4.0f) &&
					near(br.off_right, 13.0f) && near(br.off_bottom, 12.0f),
			"BR hangs off the trailing anchors");
}

void position_contract() {
	// Stage 1: missing edges read 0 [orig: the zeroed element fields].
	const opennova::mnu::RectEdges plain = opennova::mnu::position_rect(
			true, 10, true, 20, true, 110, true, 60, 0, 0);
	check(plain.left == 10 && plain.right == 110 && plain.bottom == 60,
			"authored edges pass through");
	// Stage 2: a degenerate axis falls back to the largest appearance image.
	const opennova::mnu::RectEdges img = opennova::mnu::position_rect(
			true, 10, true, 20, false, 0, false, 0, 64, 48);
	check(img.right == 74 && img.bottom == 68,
			"degenerate axes size from the image extents");
	// A negative authored left with no right edge is NOT degenerate (0 > left).
	const opennova::mnu::RectEdges neg = opennova::mnu::position_rect(
			true, -27, true, 0, false, 0, false, 0, 64, 48);
	check(neg.right == 0 && neg.left == -27,
			"a missing right edge above a negative left stays authored");
	check(opennova::mnu::appearance_extent_height(true, 32, 128) == 32,
			"the authored HEIGHT attr wins the extent");
	check(opennova::mnu::appearance_extent_height(false, 0, 128) == 128,
			"an unauthored height reads the texture");
	check(opennova::mnu::window_type_is_text_sized(opennova::mnu::WindowType::Button) &&
					opennova::mnu::window_type_is_text_sized(opennova::mnu::WindowType::Edit) &&
					!opennova::mnu::window_type_is_text_sized(opennova::mnu::WindowType::List),
			"the text-sized family is the witnessed widget set");
}

void text_adjust_contract() {
	// [orig: CStaticWnd_AdjustRectToTextSize @ 0x6575f0] — the authored point is the
	// anchor the JUSTIFY/VJUSTIFY flags align to.
	opennova::mnu::RectEdges base;
	base.left = 100;
	base.right = 100;
	base.top = 50;
	base.bottom = 50;
	const opennova::mnu::RectEdges left_j =
			opennova::mnu::adjust_rect_to_text_size(base, 40, 16, "", "");
	check(left_j.left == 100 && left_j.right == 140 && left_j.bottom == 66,
			"default justify grows right/down from the anchor");
	const opennova::mnu::RectEdges center_j =
			opennova::mnu::adjust_rect_to_text_size(base, 40, 16, "CENTER", "center");
	check(center_j.left == 80 && center_j.right == 120 &&
					center_j.top == 42 && center_j.bottom == 58,
			"center justify straddles the anchor (case-insensitive)");
	const opennova::mnu::RectEdges right_j =
			opennova::mnu::adjust_rect_to_text_size(base, 40, 16, "right", "bottom");
	check(right_j.left == 60 && right_j.right == 100 &&
					right_j.top == 34 && right_j.bottom == 50,
			"right/bottom keep the trailing edge at the anchor");
	// A non-degenerate axis is left untouched.
	opennova::mnu::RectEdges sized = base;
	sized.right = 200;
	const opennova::mnu::RectEdges keep =
			opennova::mnu::adjust_rect_to_text_size(sized, 40, 16, "center", "");
	check(keep.left == 100 && keep.right == 200,
			"an authored axis never re-sizes from text");
}

void spin_and_color_contract() {
	// [orig: @ 0x64b8b0] — authored coords parent-relative; missing far edges
	// size from the appearance extents, else the nominal arrow box.
	const opennova::mnu::RectEdges arrow = opennova::mnu::spin_button_rect(
			true, -27, true, 2, false, 0, false, 0, 0, 0);
	check(arrow.left == -27 && arrow.right == -11 && arrow.bottom == 14,
			"an extent-less arrow takes the nominal 16x12 box");
	const opennova::mnu::RectEdges sized = opennova::mnu::spin_button_rect(
			true, 56, true, 0, true, 80, true, 12, 40, 40);
	check(sized.right == 80 && sized.bottom == 12,
			"authored far edges win over the extents");

	// [orig: wcstoul base 16 @ 0x64bd10, forced opaque @ 0x64b220].
	check(opennova::mnu::item_color_argb("C08040") == 0xFFC08040u,
			"RRGGBB parses base-16 forced opaque");
	check(opennova::mnu::item_color_argb("0") == 0xFF000000u, "zero stays opaque black");
	check(opennova::mnu::item_color_argb("not-a-color") == 0xFF000000u,
			"a non-hex string reads 0 like wcstoul");
}

} // namespace

int main() {
	frame_contract();
	position_contract();
	text_adjust_contract();
	spin_and_color_contract();
	if (failures) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::puts("mnu_layout_test ok");
	return 0;
}
