// The tile tools (tile_tools.h, ADR 0046 S25 B2): the original editors' tile tools over a tile placement's rows.
#include "tile_tools.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

#include <editor/documents/til_document.h>

namespace opennova::editor {

namespace {

constexpr NodeKind kTile = node_kind(TilKind::Tile);

// The two quarter-turn tables over the low three bits, in the file's encoding [orig: gobj sub_1000EDDA @
// 0x1000EE12..0x1000EE89: a3 set, the plane's clockwise turn as the game draws the cell (MED's sub_433740); a3 clear,
// counter-clockwise (MED's sub_4336E0)]. Through the game's drawing (til_transform_local_uv, the cell's first row along
// the north edge), flags 4 shows the cell turned a quarter counter-clockwise, and 7 a quarter clockwise.
constexpr uint8_t kTurnCounterClockwise[8] = {4, 5, 6, 7, 3, 2, 1, 0};
constexpr uint8_t kTurnClockwise[8] = {7, 6, 5, 4, 0, 1, 2, 3};

// The rows of `document` the identities name, in file order, each once (a row it lacks passed over).
std::vector<const TilRow *> selected(const TilDocument &document, const std::vector<NodeId> &rows) {
	const std::set<NodeId> wanted(rows.begin(), rows.end());
	std::vector<const TilRow *> out;
	for (const auto &node : document.rows())
		if (node && node->kind == kTile && wanted.count(node->id)) out.push_back(static_cast<const TilRow *>(node.get()));
	return out;
}

NodeAddress address_of_row(const TilRow &row) { return {row.id, kTile, 0}; }

Edit set_edit(const NodeAddress &address, const char *field, const Value &value, uint64_t gesture = 0) {
	Edit edit;
	edit.operation = EditOperation::Set;
	edit.address = address;
	edit.field = field;
	edit.value = value;
	edit.gesture = gesture;
	return edit;
}

// The Sets that give `row` the record `to` (the fields that differ alone).
void sets_to(const TilRow &row, const TilOverlayEntry &to, std::vector<Edit> &out, uint64_t gesture = 0) {
	const TilOverlayEntry &from = row.entry;
	const NodeAddress at = address_of_row(row);
	if (to.x_fixed != from.x_fixed) out.push_back(set_edit(at, "x", double(to.x_fixed) / 65536.0, gesture));
	if (to.z_fixed != from.z_fixed) out.push_back(set_edit(at, "y", double(to.z_fixed) / 65536.0, gesture));
	if (to.flags != from.flags) out.push_back(set_edit(at, "orientation", int64_t(to.flags), gesture));
	if (to.tile_index != from.tile_index) out.push_back(set_edit(at, "tile", int64_t(to.tile_index), gesture));
}

// MED's step on one axis of its frame: `step` 16.16, positive rounding up first, then snapped down to whole units
// [orig: JOTACmed sub_43F0B0 @ 0x43F169..0x43F1A0].
int32_t med_step(int32_t value, int32_t step) {
	int64_t v = value;
	if (step > 0) v += 0xFFFF;
	v += step;
	return int32_t(uint32_t(v) & 0xFFFF0000u);
}

} // namespace

TileSquare tile_square(const TilOverlayEntry &entry) {
	TileSquare square;
	square.x0 = entry.x_fixed;
	square.x1 = int64_t(entry.x_fixed) + kTileSquareFixed;
	// The stored frame's [-z, -z + 16] (its second axis south) on the plane.
	square.y1 = int64_t(entry.z_fixed);
	square.y0 = square.y1 - kTileSquareFixed;
	return square;
}

size_t tile_at(const TilFile &file, double x, double y) {
	const double px = x * 65536.0, py = y * 65536.0;
	for (size_t i = file.entries.size(); i-- > 0;) {
		const TileSquare s = tile_square(file.entries[i]);
		// PtInRect over the plane's (x, y): left <= x < right, top <= y < bottom, the rectangle's top the square's
		// south edge [orig: gobj sub_10013364 @ 0x100133C9..0x100133E8; the pick point (x, +y) @ 0x10013392..0x100133AB].
		if (double(s.x0) <= px && px < double(s.x1) && double(s.y0) <= py && py < double(s.y1)) return i;
	}
	return size_t(-1);
}

std::vector<size_t> tiles_in_box(const TilFile &file, double x0, double y0, double x1, double y1) {
	// The box over the stored frame (its second axis the plane's y negated), its corners in either order.
	const double left = std::min(x0, x1) * 65536.0, right = std::max(x0, x1) * 65536.0;
	const double top = -std::max(y0, y1) * 65536.0, bottom = -std::min(y0, y1) * 65536.0;
	std::vector<size_t> out;
	for (size_t i = 0; i < file.entries.size(); ++i) {
		const TilOverlayEntry &e = file.entries[i];
		const double tl = double(e.x_fixed), tr = tl + kTileSquareFixed;
		const double tt = -double(e.z_fixed), tb = tt + kTileSquareFixed;
		// IntersectRect: a crossing of positive area on both axes.
		if (std::max(left, tl) < std::min(right, tr) && std::max(top, tt) < std::min(bottom, tb)) out.push_back(i);
	}
	return out;
}

uint8_t tile_turned(uint8_t flags, int quarter_turns) {
	const int quarters = ((quarter_turns % 4) + 4) % 4;
	const bool counter = quarter_turns > 0;
	uint8_t out = flags;
	for (int i = 0; i < (counter ? quarters : (4 - quarters) % 4); ++i)
		out = uint8_t((out & ~7u) | (counter ? kTurnCounterClockwise : kTurnClockwise)[out & 7u]);
	return out;
}

uint8_t tile_flipped(uint8_t flags, bool across) {
	// [orig: gobj sub_1000EEED @ 0x1000EF0B..0x1000EF36: across (a3 set) flips U unturned, V turned; down the other]
	const bool turned = (flags & TIL_FLAG_ROTATE_90) != 0;
	const uint8_t bit = (across != turned) ? TIL_FLAG_FLIP_X : TIL_FLAG_FLIP_Y;
	return uint8_t(flags ^ bit);
}

TilOverlayEntry tile_nudged(const TilOverlayEntry &entry, int dx, int dy, bool big) {
	// MED's step: 0x100 or 0x10000 scaled by 16 [orig: JOTACmed sub_440FD0 @ 0x44162E..0x44166B; sub_43F0B0 @
	// 0x43F0E2..0x43F0E4], so a press moves one whole unit (by the rounding) or sixteen. Its frame's second axis
	// is the stored z, the plane's y: a step north is a positive step there.
	const int32_t unit = big ? 0x100000 : 0x1000;
	TilOverlayEntry out = entry;
	if (dx) out.x_fixed = med_step(entry.x_fixed, (dx > 0 ? 1 : -1) * unit);
	if (dy) out.z_fixed = med_step(entry.z_fixed, (dy > 0 ? 1 : -1) * unit);
	return out;
}

std::vector<TilOverlayEntry> tiles_turned_about_centroid(const std::vector<TilOverlayEntry> &entries, int direction) {
	std::vector<TilOverlayEntry> out = entries;
	if (entries.empty() || direction == 0) return out;
	// MED keeps a tile by its centre, in its frame (x, the stored z less 8 units: the plane's x east and y north)
	// [orig: JOTACmed MapTileStore_Serialize @ 0x433FA0..0x433FA6]; the centroid is the centres' integer mean, the sum
	// over 64 bits divided toward zero [orig: sub_43E910 @ 0x43E9C4..0x43EA12].
	int64_t sum_x = 0, sum_s = 0;
	for (const TilOverlayEntry &e : entries) {
		sum_x += int64_t(e.x_fixed) + 0x80000;
		sum_s += int64_t(e.z_fixed) - 0x80000;
	}
	const int64_t n = int64_t(entries.size());
	const int64_t cx = sum_x / n, cs = sum_s / n;
	for (TilOverlayEntry &e : out) {
		const int64_t dx = int64_t(e.x_fixed) + 0x80000 - cx, ds = int64_t(e.z_fixed) - 0x80000 - cs;
		int64_t x = 0, s = 0;
		if (direction > 0) {
			// The plane's counter-clockwise quarter: MED's sub_4336E0 and (cx - ds, cs + dx) [orig: @ 0x43EAFE..0x43EB15].
			e.flags = tile_turned(e.flags, 1);
			x = cx - ds;
			s = dx + cs;
		} else {
			// Clockwise: sub_433740 and (cx + ds, cs - dx) [orig: @ 0x43EAC7..0x43EADE].
			e.flags = tile_turned(e.flags, -1);
			x = cx + ds;
			s = cs - dx;
		}
		// Snapped down to whole units [orig: @ 0x43EB4A..0x43EB54], back to the record's corner.
		const int32_t centre_x = int32_t(uint32_t(int32_t(x)) & 0xFFFF0000u);
		const int32_t centre_s = int32_t(uint32_t(int32_t(s)) & 0xFFFF0000u);
		e.x_fixed = centre_x - 0x80000;
		e.z_fixed = centre_s + 0x80000;
	}
	return out;
}

void tile_corner_at(double x, double y, bool grid, int32_t &x_fixed, int32_t &z_fixed) {
	if (grid) {
		// The grid square the point lies in: its west edge the multiple of 16 at or below x, its north edge the
		// multiple of 16 above y.
		x_fixed = int32_t(std::floor(x / 16.0) * 16.0) * 65536;
		z_fixed = int32_t(std::floor(y / 16.0) * 16.0 + 16.0) * 65536;
		return;
	}
	// Nile's stamp: the point's 16.16 words, truncated, plus 0x7FFF, the fraction cleared [orig: gobj sub_1000B4A1 @
	// 0x1000B4C8..0x1000B542]; the stored z is the plane's y.
	const int32_t px = int32_t(std::trunc(x * 65536.0));
	const int32_t pz = int32_t(std::trunc(y * 65536.0));
	x_fixed = int32_t(uint32_t(int64_t(px) + 0x7FFF) & 0xFFFF0000u);
	z_fixed = int32_t(uint32_t(int64_t(pz) + 0x7FFF) & 0xFFFF0000u);
}

uint8_t tile_stamp_flags(int rotation, bool flip_h, bool flip_v) {
	uint8_t flags = tile_turned(0, -rotation);
	if (flip_h) flags = tile_flipped(flags, true);
	if (flip_v) flags = tile_flipped(flags, false);
	return flags;
}

std::vector<TileCell> tile_palette(int atlas_width, int atlas_height, const TilFile *placement) {
	std::vector<TileCell> out;
	const TilAtlasLayout layout = til_make_atlas_layout(atlas_width, atlas_height);
	if (layout.tiles_x <= 0 || layout.tiles_y <= 0) return out;
	// A cell is a column's and a row's share of the set (the UV steps 1 / columns, 1 / rows [orig:
	// Terrain_LoadTileSetAtlas @ 0x604BB4..0x604BC2]), 64 texels where the set's sides are multiples of 64.
	const int cells = std::min(layout.tiles_x * layout.tiles_y, 256);
	for (int i = 0; i < cells; ++i) {
		const int col = i % layout.tiles_x, row = i / layout.tiles_x;
		TileCell cell;
		cell.index = i;
		cell.x = col * atlas_width / layout.tiles_x;
		cell.y = row * atlas_height / layout.tiles_y;
		cell.w = (col + 1) * atlas_width / layout.tiles_x - cell.x;
		cell.h = (row + 1) * atlas_height / layout.tiles_y - cell.y;
		out.push_back(cell);
	}
	if (placement)
		for (const TilOverlayEntry &e : placement->entries)
			if (e.tile_index < out.size()) ++out[e.tile_index].uses;
	return out;
}

std::vector<Edit> plan_tile_turn(const TilDocument &document, const std::vector<NodeId> &rows, int by) {
	std::vector<Edit> out;
	for (const TilRow *row : selected(document, rows)) {
		TilOverlayEntry to = row->entry;
		to.flags = tile_turned(to.flags, by);
		sets_to(*row, to, out);
	}
	return out;
}

std::vector<Edit> plan_tile_flip(const TilDocument &document, const std::vector<NodeId> &rows, bool across) {
	std::vector<Edit> out;
	for (const TilRow *row : selected(document, rows)) {
		TilOverlayEntry to = row->entry;
		to.flags = tile_flipped(to.flags, across);
		sets_to(*row, to, out);
	}
	return out;
}

std::vector<Edit> plan_tile_layer(const TilDocument &document, const std::vector<NodeId> &rows, bool to_front) {
	std::vector<Edit> out;
	const std::vector<const TilRow *> picked = selected(document, rows);
	if (picked.empty()) return out;
	// Nile walks the array in order: to the front, each selected tile moves to the end after the ones before it;
	// to the back, each to the start after the ones before it [orig: gobj sub_1000EC29 @ 0x1000EC60..0x1000ECA4,
	// @ 0x1000ED2B..0x1000EDB4]. A Move to the end appends; to the start, moving them last first keeps their order.
	const auto move = [&](const TilRow &row, size_t position) {
		Edit edit;
		edit.operation = EditOperation::Move;
		edit.address = address_of_row(row);
		edit.position = position;
		out.push_back(edit);
	};
	// Already where they go: nothing.
	const auto &all = document.rows();
	bool placed = true;
	for (size_t i = 0; i < picked.size(); ++i) {
		const size_t at = to_front ? all.size() - picked.size() + i : i;
		placed = placed && at < all.size() && all[at].get() == picked[i];
	}
	if (placed) return out;
	if (to_front)
		for (const TilRow *row : picked) move(*row, SIZE_MAX);
	else
		for (size_t i = picked.size(); i-- > 0;) move(*picked[i], 0);
	return out;
}

std::vector<Edit> plan_tile_nudge(const TilDocument &document, const std::vector<NodeId> &rows, int dx, int dy, bool big) {
	std::vector<Edit> out;
	for (const TilRow *row : selected(document, rows)) sets_to(*row, tile_nudged(row->entry, dx, dy, big), out);
	return out;
}

std::vector<Edit> plan_tile_move(const TilDocument &document, const std::vector<NodeId> &rows, int64_t dx, int64_t dy,
                                 uint64_t gesture) {
	std::vector<Edit> out;
	if (dx == 0 && dy == 0) return out;
	for (const TilRow *row : selected(document, rows)) {
		TilOverlayEntry to = row->entry;
		const int64_t x = int64_t(to.x_fixed) + dx;
		const int64_t z = int64_t(to.z_fixed) + dy;
		if (x < std::numeric_limits<int32_t>::min() || x > std::numeric_limits<int32_t>::max() ||
		    z <= std::numeric_limits<int32_t>::min() || z > std::numeric_limits<int32_t>::max())
			continue;
		to.x_fixed = int32_t(x);
		to.z_fixed = int32_t(z);
		sets_to(*row, to, out, gesture);
	}
	return out;
}

std::vector<Edit> plan_tile_group_turn(const TilDocument &document, const std::vector<NodeId> &rows, int direction) {
	std::vector<Edit> out;
	const std::vector<const TilRow *> picked = selected(document, rows);
	std::vector<TilOverlayEntry> entries;
	for (const TilRow *row : picked) entries.push_back(row->entry);
	const std::vector<TilOverlayEntry> turned = tiles_turned_about_centroid(entries, direction);
	for (size_t i = 0; i < picked.size(); ++i) sets_to(*picked[i], turned[i], out);
	return out;
}

std::vector<Edit> plan_tile_delete(const TilDocument &document, const std::vector<NodeId> &rows) {
	std::vector<Edit> out;
	for (const TilRow *row : selected(document, rows)) {
		Edit edit;
		edit.operation = EditOperation::Remove;
		edit.address = address_of_row(*row);
		out.push_back(edit);
	}
	return out;
}

std::vector<Edit> plan_tile_duplicate(const TilDocument &document, const std::vector<NodeId> &rows, int32_t dx, int32_t dy) {
	std::vector<Edit> out;
	const std::vector<const TilRow *> picked = selected(document, rows);
	if (picked.empty()) return out;
	// The copies after the last selected tile, in their order (each over the others, as a stamp is).
	size_t last = 0;
	for (size_t i = 0; i < document.rows().size(); ++i)
		if (document.rows()[i].get() == picked.back()) last = i;
	for (size_t i = 0; i < picked.size(); ++i) {
		Edit copy;
		copy.operation = EditOperation::Duplicate;
		copy.address = address_of_row(*picked[i]);
		copy.position = last + 1 + i;
		const size_t made = out.size();
		out.push_back(copy);
		if (dx || dy) {
			const NodeAddress at{batch_made(made), kTile, 0};
			const TilOverlayEntry &e = picked[i]->entry;
			if (dx) out.push_back(set_edit(at, "x", double(int64_t(e.x_fixed) + int64_t(dx) * 65536) / 65536.0));
			if (dy) out.push_back(set_edit(at, "y", double(int64_t(e.z_fixed) + int64_t(dy) * 65536) / 65536.0));
		}
	}
	return out;
}

std::vector<Edit> plan_tile_stamp(int cell, uint8_t flags, double x, double y, bool grid) {
	std::vector<Edit> out;
	if (cell < 0 || cell > 255) return out;
	int32_t x_fixed = 0, z_fixed = 0;
	tile_corner_at(x, y, grid, x_fixed, z_fixed);
	Edit add;
	add.operation = EditOperation::Add;
	add.address.kind = kTile;
	add.field = "tile";
	add.value = int64_t(cell);
	out.push_back(add);
	const NodeAddress at{batch_made(0), kTile, 0};
	out.push_back(set_edit(at, "x", double(x_fixed) / 65536.0));
	out.push_back(set_edit(at, "y", double(z_fixed) / 65536.0));
	if (flags) out.push_back(set_edit(at, "orientation", int64_t(flags)));
	return out;
}

} // namespace opennova::editor
