#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <editor/model/edit.h>
#include <formats/til/til.h>

namespace opennova::editor {

class TilDocument;

// The tile tools (ADR 0046 S25 B2; CONTEXT.md "Terrain brush"): what the Tiles tools do to a tile placement's
// tiles, each a batch of edits on its .til document (one undo step), ported from the original editors' own tools
// (docs/tiles/til-re.md "The file as the editors write it"): Nile's picking, turns, flips and layer order (gobj.dll)
// and MED's nudge and group turn (JOTACmed.exe). They are UI over the file's records: what the game draws of a tile
// is its record, which these change as the editors did.
//
// The plane is the mission's: x east, y north, in mission units. A record's corner is (x, stored z), the north-west
// corner of the square the game draws it over, spanning 16 units east and south: the page pass draws a record over
// its bake space's [x, x + 16] x [-z, -z + 16], whose second axis runs south [orig: PolyTrn_RenderTile @
// 0x60DE20..0x60DE60], the cell's first row along the north edge and its first column along the west edge, so an
// unturned tile shows its cell as the set holds it, north up. (The game's surface sampler tests the other square,
// [-z, -z + 16] north: preview/mission_ground_facts reads it as the game does; docs/tiles/til-re.md D-TIL-9.) A
// turn's direction is the plane's seen from above, north up: positive counter-clockwise.

inline constexpr int32_t kTileSquareFixed = TIL_FIXED_CELL_UNITS; // 16 units, 16.16

// A tile's square on the plane, 16.16: [x0, x1) east, [y0, y1) north (y1 the stored z), as Nile's click tests it
// (PtInRect over the plane's (x, y), the rectangle's top its south edge [orig: gobj sub_10013364 @
// 0x100133C9..0x100133E8]).
struct TileSquare {
	int64_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
TileSquare tile_square(const TilOverlayEntry &entry);

// The tile a click at (x, y) (mission units) picks: of the tiles whose square holds it, the one drawn on top (the
// last in file order; Nile's click takes the first, which the editor's views draw under the others), npos for none.
size_t tile_at(const TilFile &file, double x, double y);
// The tiles a box (two corners, mission units) selects: every tile whose square it crosses, in file order (Nile's
// drag box: IntersectRect over the stored frame, an edge alone not crossing [orig: gobj sub_10013416 @
// 0x100134B8..0x100134DE]).
std::vector<size_t> tiles_in_box(const TilFile &file, double x0, double y0, double x1, double y1);

// The flags byte turned `quarter_turns` quarters (positive counter-clockwise on the plane), each quarter one row of
// Nile's table over the low three bits, the others kept [orig: gobj sub_1000EDDA @ 0x1000EE12..0x1000EE89; MED's
// sub_433740 / sub_4336E0 are the same tables in its own encoding].
uint8_t tile_turned(uint8_t flags, int quarter_turns);
// The flags byte mirrored across (east to west) or down (north to south) as the plane shows it: Nile's rule, which
// flips the cell's U on an unturned tile and its V on a turned one [orig: gobj sub_1000EEED @ 0x1000EF0B..0x1000EF36].
uint8_t tile_flipped(uint8_t flags, bool across);

// A record nudged (dx, dy) steps on the plane (each -1, 0 or 1), a step 1 unit or, `big`, 16: MED's arrow keys,
// each coordinate snapped down to whole units after the move, a positive move rounding the coordinate up first
// [orig: JOTACmed sub_43F0B0 @ 0x43F0DE..0x43F1A7, its second axis the stored z, the plane's y, positive north].
TilOverlayEntry tile_nudged(const TilOverlayEntry &entry, int dx, int dy, bool big);
// Records turned a quarter (positive counter-clockwise) about their tiles' centroid, each turned in place too:
// MED's group turn over the tiles' centres, the centroid their integer mean, each centre's offset turned and the
// positions snapped down to whole units [orig: JOTACmed sub_43E910 @ 0x43E977..0x43EB54].
std::vector<TilOverlayEntry> tiles_turned_about_centroid(const std::vector<TilOverlayEntry> &entries, int direction);
// The corner a stamp at (x, y) (mission units) places, the square's north-west: Nile's, the point to the nearest whole
// unit [orig: gobj sub_1000B4A1 @ 0x1000B4C8..0x1000B542]; `grid` takes the north-west corner of the 16-unit grid
// square the point lies in instead (the editor's own snap). False (the words untouched) where the corner lies past
// the record's 16.16 range, beyond 32767 units either way.
bool tile_corner_at(double x, double y, bool grid, int32_t &x_fixed, int32_t &z_fixed);

// The tile set's palette: its cells as the game cuts them (the width over 64 columns, one at least; the height over
// 64 rows; each cell a column's and a row's share of the set) [orig: Terrain_LoadTileSetAtlas @ 0x604B8A..0x604BC2],
// an index the cell's column plus its row times the columns [orig: PolyTrn_RenderTile @ 0x60DEBF], 256 at most (a
// record's byte), each its texels in the set and how many tiles of `placement` (none: 0 each) draw it.
struct TileCell {
	int index = 0;
	int x = 0, y = 0, w = 0, h = 0;
	size_t uses = 0;
};
std::vector<TileCell> tile_palette(int atlas_width, int atlas_height, const TilFile *placement = nullptr);

// --- the planners: a batch of edits on the .til document, empty where nothing changes ---------------------------
// Each names its rows by identity (the document's rows), in any order; a row the document lacks is passed over.

// The selected tiles turned a quarter each in place (Nile's R), `by` quarters.
std::vector<Edit> plan_tile_turn(const TilDocument &document, const std::vector<NodeId> &rows, int by);
// The selected tiles mirrored across or down, each in place (Nile's flip).
std::vector<Edit> plan_tile_flip(const TilDocument &document, const std::vector<NodeId> &rows, bool across);
// The selected tiles moved to the end of the file (drawn over every other: `to_front`) or its start, in their order:
// Nile's layer up and down [orig: gobj sub_1000EC29 @ 0x1000EC39..0x1000EDC8]; each takes its new neighbour's layer
// (TilDocument::settle_placed_rows).
std::vector<Edit> plan_tile_layer(const TilDocument &document, const std::vector<NodeId> &rows, bool to_front);
// The selected tiles nudged (dx, dy) steps (MED's arrows).
std::vector<Edit> plan_tile_nudge(const TilDocument &document, const std::vector<NodeId> &rows, int dx, int dy, bool big);
// The selected tiles moved by (dx, dy), 16.16 units east and north (a drag's step), `gesture` folding a drag's
// batches into one step; none where the move would carry a tile past the stored 16.16 range (refused whole, the
// group kept together).
std::vector<Edit> plan_tile_move(const TilDocument &document, const std::vector<NodeId> &rows, int64_t dx, int64_t dy,
                                 uint64_t gesture = 0);
// The selected tiles turned a quarter together about their centroid (MED's group turn), `direction` its sign.
std::vector<Edit> plan_tile_group_turn(const TilDocument &document, const std::vector<NodeId> &rows, int direction);
// The selected tiles removed.
std::vector<Edit> plan_tile_delete(const TilDocument &document, const std::vector<NodeId> &rows);
// The selected tiles copied, each right after its last copy, offset (dx, dy) whole units.
std::vector<Edit> plan_tile_duplicate(const TilDocument &document, const std::vector<NodeId> &rows, int32_t dx = 0,
                                      int32_t dy = 0);
// A tile of `cell` stamped with its corner at (x, y) (tile_corner_at), its flags `flags`, after the last tile (drawn
// over the others).
std::vector<Edit> plan_tile_stamp(int cell, uint8_t flags, double x, double y, bool grid);
// The flags byte a stamp lays: the brush's quarter turns (clockwise on the plane, as the views show it north up)
// and flips, from an unturned tile.
uint8_t tile_stamp_flags(int rotation, bool flip_h, bool flip_v);

} // namespace opennova::editor
