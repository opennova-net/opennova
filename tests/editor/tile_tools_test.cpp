// The tile tools (ADR 0046 S25 B2; editor/preview/tile_tools.h): the original editors' tile tools over a tile
// placement's rows. Nile's turn tables (in the file's encoding, MED's two tables translated) and its flip rule, each
// checked against the game's own drawing of a tile's flags (formats/til's til_transform_local_uv, D-TIL-4): a turn
// is a quarter of the drawn tile about its centre on the plane, a flip its mirror; Nile's click (the square, the top
// tile) and drag box; MED's nudge (a whole unit or 16, a positive move rounding up), its group turn about the
// centroid; Nile's stamp corner (the nearest whole unit) and the grid's; each planner's batch one undo step on the
// .til document; the layer order (to the front, to the back) and the layers settled; a stamp, a duplicate, a delete;
// the palette's cells.
#include <editor/documents/til_document.h>
#include <editor/preview/tile_tools.h>
#include <formats/til/til_io.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "common/test_expect.h"

using namespace opennova;
using namespace opennova::editor;

namespace {

constexpr NodeKind kTile = node_kind(TilKind::Tile);

// The drawn art of a tile of `flags` at its local point (x east, y north over its square, 0..1), as the game samples
// its cell (til_transform_local_uv: the local x as U, the local y as V before the flags).
TilUv drawn(uint8_t flags, double x, double y) { return til_transform_local_uv(TilUv{float(x), float(y)}, flags); }

bool same(TilUv a, TilUv b) { return std::fabs(a.u - b.u) < 1e-6f && std::fabs(a.v - b.v) < 1e-6f; }

int test_turns_and_flips() {
	// Nile's tables, the file's encoding: counter-clockwise on the plane 0>7 1>6 2>5 3>4 4>0 5>1 6>2 7>3, clockwise
	// 0>4 1>5 2>6 3>7 4>3 5>2 6>1 7>0 [gobj sub_1000EDDA].
	const uint8_t ccw[8] = {7, 6, 5, 4, 0, 1, 2, 3}, cw[8] = {4, 5, 6, 7, 3, 2, 1, 0};
	for (uint8_t f = 0; f < 8; ++f) {
		TEST_EXPECT(tile_turned(f, 1) == ccw[f] && tile_turned(f, -1) == cw[f]);
		// The other bits kept: the selection mark, a bit no tool writes.
		TEST_EXPECT(tile_turned(uint8_t(f | 0x88), 1) == (ccw[f] | 0x88));
		TEST_EXPECT(tile_turned(f, 4) == f && tile_turned(f, -4) == f && tile_turned(tile_turned(f, 1), -1) == f);
		TEST_EXPECT(tile_turned(f, 2) == tile_turned(tile_turned(f, 1), 1) && tile_turned(f, 3) == tile_turned(f, -1));
		// The game's drawing: the turned tile shows at each point what the tile showed a quarter clockwise of it
		// (its art turned counter-clockwise about the centre, x east, y north).
		for (const double x : {0.1, 0.4, 0.8})
			for (const double y : {0.2, 0.7}) {
				TEST_EXPECT(same(drawn(ccw[f], x, y), drawn(f, y, 1.0 - x)));
				TEST_EXPECT(same(drawn(cw[f], x, y), drawn(f, 1.0 - y, x)));
				// A flip across mirrors the art east to west, down north to south, turned or not (Nile's rule).
				TEST_EXPECT(same(drawn(tile_flipped(f, true), x, y), drawn(f, 1.0 - x, y)));
				TEST_EXPECT(same(drawn(tile_flipped(f, false), x, y), drawn(f, x, 1.0 - y)));
			}
		TEST_EXPECT(tile_flipped(tile_flipped(f, true), true) == f);
	}
	// A stamp's flags: the brush's quarters (clockwise), then its flips.
	TEST_EXPECT(tile_stamp_flags(0, false, false) == 0 && tile_stamp_flags(1, false, false) == 4 &&
	            tile_stamp_flags(3, false, false) == 7 && tile_stamp_flags(0, true, false) == 1 &&
	            tile_stamp_flags(1, true, false) == tile_flipped(4, true));
	std::printf("turns and flips: Nile's tables and flip rule, each the game's drawing turned or mirrored\n");
	return 0;
}

int test_picking() {
	TilFile file;
	file.entries = {make_til_entry(0, 0, 1, 0, 0), make_til_entry(8 << 16, -(8 << 16), 2, 0, 0),
	                make_til_entry(100 << 16, -(100 << 16), 3, 0, 0)};
	// The square: [x, x + 16) east, (y, y + 16] north.
	TEST_EXPECT(tile_at(file, 4.0, 4.0) == 0 && tile_at(file, 10.0, 10.0) == 1);
	TEST_EXPECT(tile_at(file, 0.0, 0.0) == size_t(-1) && tile_at(file, 0.0, 0.001) == 0);
	TEST_EXPECT(tile_at(file, 15.999, 16.0) == 1 && tile_at(file, 16.0, 16.0) == 1 && tile_at(file, 24.0, 8.5) == size_t(-1));
	// The drag box: every square it crosses with area; an edge alone does not cross.
	TEST_EXPECT(tiles_in_box(file, 2, 2, 12, 12) == std::vector<size_t>({0, 1}));
	TEST_EXPECT(tiles_in_box(file, 16, 0, 20, 4).empty() && tiles_in_box(file, 120, 120, 90, 90) == std::vector<size_t>({2}));
	std::printf("picking: the square, the top tile, the drag box\n");
	return 0;
}

int test_nudge_turn_corner() {
	// MED's arrows: a whole unit, 16 with the modifier; a positive move rounds up first, a negative down.
	const TilOverlayEntry whole = make_til_entry(5 << 16, -(10 << 16), 0, 0, 0);
	TEST_EXPECT(tile_nudged(whole, 1, 0, false).x_fixed == (6 << 16) && tile_nudged(whole, -1, 0, false).x_fixed == (4 << 16));
	TEST_EXPECT(tile_nudged(whole, 1, 0, true).x_fixed == (21 << 16) && tile_nudged(whole, -1, 0, true).x_fixed == -(11 << 16));
	TEST_EXPECT(tile_nudged(whole, 0, 1, false).z_fixed == -(11 << 16) && tile_nudged(whole, 0, -1, false).z_fixed == -(9 << 16));
	const TilOverlayEntry half = make_til_entry((5 << 16) + 0x8000, 0, 0, 0, 0);
	TEST_EXPECT(tile_nudged(half, 1, 0, false).x_fixed == (6 << 16) && tile_nudged(half, -1, 0, false).x_fixed == (5 << 16));
	TEST_EXPECT(tile_nudged(half, 1, 0, true).x_fixed == (22 << 16) && tile_nudged(half, -1, 0, true).x_fixed == -(11 << 16));
	// MED's group turn: two tiles side by side turned a quarter counter-clockwise about their centroid: the west one
	// goes south, the east one north, each turned in place.
	const std::vector<TilOverlayEntry> pair = {make_til_entry(0, 0, 4, 0, 0), make_til_entry(32 << 16, 0, 4, 0, 0)};
	const std::vector<TilOverlayEntry> turned = tiles_turned_about_centroid(pair, 1);
	TEST_EXPECT(turned[0].x_fixed == (16 << 16) && turned[0].z_fixed == (16 << 16) && turned[0].flags == 7);
	TEST_EXPECT(turned[1].x_fixed == (16 << 16) && turned[1].z_fixed == -(16 << 16) && turned[1].flags == 7);
	const std::vector<TilOverlayEntry> back = tiles_turned_about_centroid(turned, -1);
	TEST_EXPECT(back[0].x_fixed == 0 && back[0].z_fixed == 0 && back[1].x_fixed == (32 << 16) && back[0].flags == 0);
	// Nile's stamp: the corner the nearest whole unit; the grid's the corner of the square the point lies in.
	int32_t x = 0, z = 0;
	tile_corner_at(10.4, 5.6, false, x, z);
	TEST_EXPECT(x == (10 << 16) && z == -(6 << 16));
	tile_corner_at(10.6, -3.4, false, x, z);
	TEST_EXPECT(x == (11 << 16) && z == (3 << 16));
	tile_corner_at(10.6, 23.0, true, x, z);
	TEST_EXPECT(x == 0 && z == -(16 << 16));
	tile_corner_at(-0.5, -16.5, true, x, z);
	TEST_EXPECT(x == -(16 << 16) && z == (32 << 16));
	std::printf("nudge, group turn, corner: MED's steps and centroid, Nile's rounding\n");
	return 0;
}

std::vector<uint8_t> four_tiles() {
	TilFile file;
	file.entries = {make_til_entry(0, 0, 1, 0, 0), make_til_entry(16 << 16, 0, 2, 0x08, 0), make_til_entry(32 << 16, 0, 3, 4, 1),
	                make_til_entry(48 << 16, 0, 4, 0, 3)};
	std::vector<uint8_t> out;
	std::string why;
	save_til(file, out, why);
	return out;
}

std::vector<uint8_t> indexes(const TilDocument &document) {
	std::vector<uint8_t> out;
	for (const TilOverlayEntry &e : document.file().entries) out.push_back(e.tile_index);
	return out;
}

std::vector<uint16_t> layers(const TilDocument &document) {
	std::vector<uint16_t> out;
	for (const TilOverlayEntry &e : document.file().entries) out.push_back(e.layer);
	return out;
}

int test_planners() {
	TilDocument document;
	Diagnostic error;
	const std::vector<uint8_t> bytes = four_tiles();
	TEST_EXPECT(document.load_bytes(bytes, "a.til", AssetKind::TileInfo, "jo", error));
	const auto id = [&](size_t i) { return document.rows()[i]->id; };
	const auto apply = [&](const std::vector<Edit> &edits) { return !edits.empty() && document.apply(edits, error); };
	const uint64_t revision = document.revision();
	// A turn of two tiles: one step; their flags turned, the selection mark kept.
	TEST_EXPECT(apply(plan_tile_turn(document, {id(1), id(2)}, 1)));
	TEST_EXPECT(document.file().entries[1].flags == (0x08 | 7) && document.file().entries[2].flags == 0);
	document.undo();
	TEST_EXPECT(document.serialize().text == std::string(bytes.begin(), bytes.end()) && document.revision() == revision);
	// A flip across, a nudge north, a drag east: each a step.
	TEST_EXPECT(apply(plan_tile_flip(document, {id(0)}, true)) && document.file().entries[0].flags == 1);
	TEST_EXPECT(apply(plan_tile_nudge(document, {id(0)}, 0, 1, true)) && document.file().entries[0].z_fixed == -(16 << 16));
	const uint64_t gesture = next_edit_gesture();
	TEST_EXPECT(apply(plan_tile_move(document, {id(0)}, 3 << 16, 0, gesture)) &&
	            apply(plan_tile_move(document, {id(0)}, 2 << 16, 0, gesture)));
	TEST_EXPECT(document.file().entries[0].x_fixed == (5 << 16));
	document.end_edit_group();
	document.undo();
	TEST_EXPECT(document.file().entries[0].x_fixed == 0 && document.file().entries[0].z_fixed == -(16 << 16));
	document.undo();
	document.undo();
	TEST_EXPECT(document.serialize().text == std::string(bytes.begin(), bytes.end()));
	// To the front: tiles 1 and 2 drawn last, in their order, each the layer of the row before it (3).
	TEST_EXPECT(apply(plan_tile_layer(document, {id(2), id(0)}, true)));
	TEST_EXPECT(indexes(document) == std::vector<uint8_t>({2, 4, 1, 3}) && layers(document) == std::vector<uint16_t>({0, 3, 3, 3}));
	TEST_EXPECT(plan_tile_layer(document, {id(2), id(3)}, true).empty());
	// To the back: the last two first, in their order, each the layer of the row after the first (0).
	TEST_EXPECT(apply(plan_tile_layer(document, {id(2), id(3)}, false)));
	TEST_EXPECT(indexes(document) == std::vector<uint8_t>({1, 3, 2, 4}) && layers(document) == std::vector<uint16_t>({0, 0, 0, 3}));
	document.undo();
	document.undo();
	TEST_EXPECT(document.serialize().text == std::string(bytes.begin(), bytes.end()));
	// A group turn: one step.
	TEST_EXPECT(apply(plan_tile_group_turn(document, {id(0), id(1)}, 1)));
	TEST_EXPECT(document.file().entries[0].x_fixed == (8 << 16) && document.file().entries[0].z_fixed == (8 << 16));
	document.undo();
	// A stamp over the others: cell 9 at the corner Nile places it, after the last tile with its layer.
	// (The brush's three quarters clockwise: one counter-clockwise.)
	TEST_EXPECT(apply(plan_tile_stamp(9, tile_stamp_flags(3, false, false), 70.3, -4.6, false)));
	const TilOverlayEntry stamped = document.file().entries.back();
	TEST_EXPECT(stamped.tile_index == 9 && stamped.x_fixed == (70 << 16) && stamped.z_fixed == (5 << 16) && stamped.flags == 7 &&
	            stamped.layer == 3);
	document.undo();
	// A duplicate of two, offset 16 east: the copies after the last of them, in their order.
	TEST_EXPECT(apply(plan_tile_duplicate(document, {id(0), id(1)}, 16, 0)));
	TEST_EXPECT(indexes(document) == std::vector<uint8_t>({1, 2, 1, 2, 3, 4}) && document.file().entries[2].x_fixed == (16 << 16) &&
	            document.file().entries[3].x_fixed == (32 << 16) && layers(document) == std::vector<uint16_t>({0, 0, 0, 0, 1, 3}));
	document.undo();
	// A delete of every tile: the header alone.
	TEST_EXPECT(apply(plan_tile_delete(document, {id(0), id(1), id(2), id(3)})) && document.rows().empty());
	TEST_EXPECT(document.serialize().text.size() == 16);
	document.undo();
	TEST_EXPECT(document.serialize().text == std::string(bytes.begin(), bytes.end()));
	// Nothing to do: no batch.
	TEST_EXPECT(plan_tile_move(document, {id(0)}, 0, 0).empty() && plan_tile_turn(document, {}, 1).empty());
	std::printf("planners: each a step, the layers settled, a stamp over the others, a duplicate, a delete\n");
	return 0;
}

int test_palette() {
	// A 512 x 512 set: 64 cells of 64; the uses counted.
	TilFile file;
	file.entries = {make_til_entry(0, 0, 9, 0, 0), make_til_entry(0, 0, 9, 0, 0), make_til_entry(0, 0, 63, 0, 0)};
	const std::vector<TileCell> cells = tile_palette(512, 512, &file);
	TEST_EXPECT(cells.size() == 64 && cells[9].x == 64 && cells[9].y == 64 && cells[9].w == 64 && cells[9].uses == 2 &&
	            cells[63].uses == 1 && cells[0].uses == 0);
	// A set 32 wide: one column as wide as itself; a set 200 wide: three columns, each a third.
	const std::vector<TileCell> thin = tile_palette(32, 128);
	TEST_EXPECT(thin.size() == 2 && thin[1].x == 0 && thin[1].y == 64 && thin[1].w == 32);
	const std::vector<TileCell> odd = tile_palette(200, 64);
	TEST_EXPECT(odd.size() == 3 && odd[1].x == 66 && odd[1].w == 67 && odd[2].x + odd[2].w == 200);
	TEST_EXPECT(tile_palette(64, 32).empty());
	std::printf("palette: the cells as the game cuts them, the uses\n");
	return 0;
}

} // namespace

int main() {
	const int failures = test_turns_and_flips() + test_picking() + test_nudge_turn_corner() + test_planners() + test_palette();
	if (failures == 0) std::printf("editor_tile_tools: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
