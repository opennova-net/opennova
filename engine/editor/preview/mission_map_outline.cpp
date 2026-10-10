#include <editor/preview/mission_map_outline.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <unordered_set>

#include <editor/assets/project_asset_source.h>
#include <editor/graph/asset_graph.h>
#include <editor/session/view/session_view.h>
#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi/threedi_strip_decode.h>

namespace opennova::editor {

namespace {

// A face looks up where its normal's up word is past this (a slope under 84 degrees); two faces that both look up
// meet at a ridge or a valley where their normals part by more than 30 degrees; two that fold back on each other (a
// fin, a sheet drawn from both sides) part by more than 150.
constexpr double kFacesUp = 0.1;
constexpr double kRidgeCos = 0.866;
constexpr double kFoldCos = -0.866;
// A point's place is read to a millimetre; an edge seen from above to a centimetre.
constexpr double kPointQuantum = 1000.0;
constexpr double kEdgeQuantum = 100.0;
constexpr double kEdgeShortest = 0.01;

struct Key3 {
	int64_t x, y, z;
	bool operator==(const Key3 &o) const { return x == o.x && y == o.y && z == o.z; }
};
struct Key3Hash {
	size_t operator()(const Key3 &k) const {
		uint64_t h = uint64_t(k.x) * 0x9E3779B97F4A7C15ull;
		h ^= uint64_t(k.y) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
		h ^= uint64_t(k.z) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
		return size_t(h);
	}
};
struct Key4 {
	int64_t a, b, c, d;
	bool operator==(const Key4 &o) const { return a == o.a && b == o.b && c == o.c && d == o.d; }
};
struct Key4Hash {
	size_t operator()(const Key4 &k) const {
		uint64_t h = uint64_t(k.a) * 0x9E3779B97F4A7C15ull;
		for (const int64_t v : { k.b, k.c, k.d }) h ^= uint64_t(v) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
		return size_t(h);
	}
};

double cross(const double o[2], const double a[2], const double b[2]) {
	return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
}

// The convex hull of `points` ((x, y) pairs), counter-clockwise, no point twice (Andrew's monotone chain).
std::vector<double> convex_hull(std::vector<std::array<double, 2>> points) {
	std::sort(points.begin(), points.end());
	points.erase(std::unique(points.begin(), points.end()), points.end());
	std::vector<double> out;
	if (points.size() < 3) {
		for (const auto &p : points) {
			out.push_back(p[0]);
			out.push_back(p[1]);
		}
		return out;
	}
	std::vector<std::array<double, 2>> hull(points.size() * 2);
	size_t k = 0;
	for (size_t i = 0; i < points.size(); ++i) {
		while (k >= 2 && cross(hull[k - 2].data(), hull[k - 1].data(), points[i].data()) <= 0.0) --k;
		hull[k++] = points[i];
	}
	for (size_t i = points.size() - 1, t = k + 1; i > 0; --i) {
		while (k >= t && cross(hull[k - 2].data(), hull[k - 1].data(), points[i - 1].data()) <= 0.0) --k;
		hull[k++] = points[i - 1];
	}
	hull.resize(k - 1);
	for (const auto &p : hull) {
		out.push_back(p[0]);
		out.push_back(p[1]);
	}
	return out;
}

double signed_area(const std::vector<double> &hull) {
	double twice = 0.0;
	const size_t n = hull.size() / 2;
	for (size_t i = 0; i < n; ++i) {
		const size_t j = (i + 1) % n;
		twice += hull[2 * i] * hull[2 * j + 1] - hull[2 * j] * hull[2 * i + 1];
	}
	return twice * 0.5;
}

double segment_distance(double x, double y, double ax, double ay, double bx, double by) {
	const double dx = bx - ax, dy = by - ay, length = dx * dx + dy * dy;
	double t = length > 0.0 ? ((x - ax) * dx + (y - ay) * dy) / length : 0.0;
	t = std::clamp(t, 0.0, 1.0);
	const double px = ax + t * dx - x, py = ay + t * dy - y;
	return std::sqrt(px * px + py * py);
}

} // namespace

void mission_vertex_words(const float position[3], double out[3]) {
	out[0] = position[2];
	out[1] = -position[0];
	out[2] = position[1];
}

namespace {

// The words a view looks along (`v`) and the two it sees (`p`, `q`): from above the up word, forward and left seen; from
// the side the left word, forward and up seen.
struct Axes {
	int v, p, q;
};
Axes axes_of(MissionOutlineView view) {
	return view == MissionOutlineView::Side ? Axes{ 1, 0, 2 } : Axes{ 2, 0, 1 };
}

// The plan (or the elevation) of one LOD's mesh (mission_model_outline's rules).
bool outline_of_lod(const threedi::Threedi3di3 &model, size_t lod_index, MissionOutlineView view,
		MissionModelOutline &out) {
	out = MissionModelOutline();
	out.lod = int(lod_index);
	out.view = view;
	const Axes axes = axes_of(view);
	const threedi::ThreediLod &lod = model.lods[lod_index];
	if (lod.vertices.items == nullptr || lod.indices.indices == nullptr || lod.strips == nullptr) return false;
	// The distinct points, and the triangles over them.
	std::unordered_map<Key3, uint32_t, Key3Hash> ids;
	std::vector<std::array<double, 3>> points;
	std::vector<std::array<uint32_t, 3>> triangles;
	const auto intern = [&](const threedi::ThreediVertex &vertex) {
		double words[3];
		mission_vertex_words(vertex.position, words);
		const Key3 key{ std::llround(words[0] * kPointQuantum), std::llround(words[1] * kPointQuantum),
			std::llround(words[2] * kPointQuantum) };
		const auto found = ids.emplace(key, uint32_t(points.size()));
		if (found.second) points.push_back({ words[0], words[1], words[2] });
		return found.first->second;
	};
	std::vector<uint16_t> decoded;
	for (size_t s = 0; s < lod.strip_count; ++s) {
		const threedi::ThreediTriangleStrip &strip = lod.strips[s];
		if (!threedi::threedi_decode_strip_indices(lod, strip, decoded)) continue;
		for (size_t i = 0; i + 2 < decoded.size(); i += 3) {
			uint32_t corner[3];
			bool inside = true;
			for (int k = 0; k < 3; ++k) {
				const size_t at = size_t(strip.start_vertex) + decoded[i + size_t(k)];
				if (strip.start_vertex < 0 || at >= lod.vertices.count) {
					inside = false;
					break;
				}
				corner[k] = intern(lod.vertices.items[at]);
			}
			if (!inside || corner[0] == corner[1] || corner[1] == corner[2] || corner[0] == corner[2]) continue;
			triangles.push_back({ corner[0], corner[1], corner[2] });
		}
	}
	if (triangles.empty()) return false;
	// The edges: one two triangles in one plane share is dropped (a quad's diagonal).
	struct Edge {
		int count = 0;
		double normal[3] = { 0.0, 0.0, 0.0 };
		bool kept = false; // the edge as the plan draws it
	};
	std::unordered_map<uint64_t, Edge> edges;
	std::vector<uint64_t> order; // the edges in the order met, so the outline is the same each read
	for (const auto &t : triangles) {
		const auto &a = points[t[0]], &b = points[t[1]], &c = points[t[2]];
		const double u[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, v[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
		double n[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
		const double length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
		if (length <= 0.0) continue;
		for (double &axis : n) axis /= length;
		for (int k = 0; k < 3; ++k) {
			const uint32_t p = t[size_t(k)], q = t[size_t((k + 1) % 3)];
			const uint64_t key = (uint64_t(std::min(p, q)) << 32) | uint64_t(std::max(p, q));
			const auto found = edges.emplace(key, Edge());
			Edge &edge = found.first->second;
			if (found.second) {
				order.push_back(key);
				for (int axis = 0; axis < 3; ++axis) edge.normal[axis] = n[axis];
			} else if (edge.count == 1) {
				const bool up_a = edge.normal[axes.v] > kFacesUp, up_b = n[axes.v] > kFacesUp;
				const double cos = edge.normal[0] * n[0] + edge.normal[1] * n[1] + edge.normal[2] * n[2];
				edge.kept = up_a != up_b || (up_a && cos < kRidgeCos) || cos < kFoldCos;
			}
			++edge.count;
		}
	}
	// Those kept, seen from above while level: an upright one (no length) dropped, one landing on another drawn once.
	std::unordered_set<Key4, Key4Hash> seen;
	for (const uint64_t key : order) {
		const Edge &edge = edges[key];
		if (edge.count == 2 && !edge.kept) continue;
		const auto &a = points[size_t(key >> 32)], &b = points[size_t(key & 0xFFFFFFFFu)];
		const double dx = b[size_t(axes.p)] - a[size_t(axes.p)], dy = b[size_t(axes.q)] - a[size_t(axes.q)];
		if (dx * dx + dy * dy < kEdgeShortest * kEdgeShortest) continue;
		int64_t ax = std::llround(a[size_t(axes.p)] * kEdgeQuantum), ay = std::llround(a[size_t(axes.q)] * kEdgeQuantum);
		int64_t bx = std::llround(b[size_t(axes.p)] * kEdgeQuantum), by = std::llround(b[size_t(axes.q)] * kEdgeQuantum);
		if (std::make_pair(bx, by) < std::make_pair(ax, ay)) {
			std::swap(ax, bx);
			std::swap(ay, by);
		}
		if (!seen.insert(Key4{ ax, ay, bx, by }).second) continue;
		for (const auto *end : { &a, &b })
			for (int axis = 0; axis < 3; ++axis) out.edges.push_back(float((*end)[size_t(axis)]));
	}
	out.points.reserve(points.size() * 3);
	std::vector<std::array<double, 2>> level;
	level.reserve(points.size());
	for (const auto &p : points) {
		for (int axis = 0; axis < 3; ++axis) out.points.push_back(float(p[size_t(axis)]));
		const double seen[2] = { p[size_t(axes.p)], p[size_t(axes.q)] };
		level.push_back({ seen[0], seen[1] });
		out.reach = std::max(out.reach, float(std::sqrt(seen[0] * seen[0] + seen[1] * seen[1])));
	}
	out.triangles.reserve(triangles.size() * 9);
	for (const auto &t : triangles)
		for (const uint32_t corner : t)
			for (int axis = 0; axis < 3; ++axis) out.triangles.push_back(float(points[corner][size_t(axis)]));
	for (const double word : convex_hull(std::move(level))) out.hull.push_back(float(word));
	return true;
}

} // namespace

bool mission_model_outline(const threedi::Threedi3di3 &model, MissionModelOutline &out, MissionOutlineView view) {
	out = MissionModelOutline();
	if (model.lods == nullptr || model.lod_count == 0) return false;
	const Axes axes = axes_of(view);
	// LOD 0's plan where it has an edge and kMissionOutlineEdgesMax or fewer, else the first coarser LOD's that has,
	// else the one with the fewest edges past none.
	bool any = false, edged = false;
	for (size_t lod = 0; lod < model.lod_count; ++lod) {
		MissionModelOutline plan;
		if (!outline_of_lod(model, lod, view, plan)) continue;
		const bool has = !plan.edges.empty();
		if (!any || (has && (!edged || plan.edges.size() < out.edges.size()))) {
			out = std::move(plan);
			edged = has;
		}
		any = true;
		if (edged && out.edges.size() / 6 <= kMissionOutlineEdgesMax) break;
	}
	// A model whose every plan is empty (upright cards alone) is drawn by its footprint's outline.
	if (any && out.edges.empty()) {
		const size_t n = out.hull.size() / 2;
		for (size_t i = 0; n >= 2 && i < n; ++i) {
			const size_t j = (i + 1) % n;
			float a[3] = { 0.0f, 0.0f, 0.0f }, b[3] = { 0.0f, 0.0f, 0.0f };
			a[axes.p] = out.hull[2 * i];
			a[axes.q] = out.hull[2 * i + 1];
			b[axes.p] = out.hull[2 * j];
			b[axes.q] = out.hull[2 * j + 1];
			out.edges.insert(out.edges.end(), { a[0], a[1], a[2], b[0], b[1], b[2] });
			if (n == 2) break;
		}
	}
	return any;
}

// --- the footprint ---------------------------------------------------------------------------------

void MissionMapFootprint::place(const float words[3], double &x, double &y) const {
	x = origin[0] + basis[0][0] * words[0] + basis[0][1] * words[1] + basis[0][2] * words[2];
	y = origin[1] + basis[1][0] * words[0] + basis[1][1] * words[1] + basis[1][2] * words[2];
}

bool MissionMapFootprint::covers(double x, double y) const {
	if (!outline) return false;
	const double dx = x - centre[0], dy = y - centre[1];
	if (dx * dx + dy * dy > reach * reach) return false;
	const std::vector<float> &t = outline->triangles;
	for (size_t i = 0; i + 8 < t.size(); i += 9) {
		double p[3][2];
		for (int k = 0; k < 3; ++k) place(&t[i + size_t(k) * 3], p[k][0], p[k][1]);
		const double at[2] = { x, y };
		const double d0 = cross(p[0], p[1], at), d1 = cross(p[1], p[2], at), d2 = cross(p[2], p[0], at);
		const bool negative = d0 < 0.0 || d1 < 0.0 || d2 < 0.0, positive = d0 > 0.0 || d1 > 0.0 || d2 > 0.0;
		// Inside (or on an edge) where the three agree; a triangle seen edge-on has no inside.
		if (!(negative && positive) && cross(p[0], p[1], p[2]) != 0.0) return true;
	}
	return false;
}

double MissionMapFootprint::distance(double x, double y) const {
	const size_t n = hull.size() / 2;
	if (n == 0) return std::hypot(x - origin[0], y - origin[1]);
	if (n == 1) return std::hypot(x - hull[0], y - hull[1]);
	bool inside = n >= 3;
	double nearest = INFINITY;
	for (size_t i = 0; i < n; ++i) {
		const size_t j = (i + 1) % n;
		const double a[2] = { hull[2 * i], hull[2 * i + 1] }, b[2] = { hull[2 * j], hull[2 * j + 1] }, at[2] = { x, y };
		if (cross(a, b, at) < 0.0) inside = false;
		nearest = std::min(nearest, segment_distance(x, y, a[0], a[1], b[0], b[1]));
	}
	return inside ? 0.0 : nearest;
}

bool MissionMapFootprint::meets(double x0, double y0, double x1, double y1) const {
	const size_t n = hull.size() / 2;
	if (n == 0) return false;
	// The separating axes of two convex shapes: the box's two, then each hull edge's normal.
	double lo[2] = { INFINITY, INFINITY }, hi[2] = { -INFINITY, -INFINITY };
	for (size_t i = 0; i < n; ++i)
		for (int axis = 0; axis < 2; ++axis) {
			lo[axis] = std::min(lo[axis], hull[2 * i + size_t(axis)]);
			hi[axis] = std::max(hi[axis], hull[2 * i + size_t(axis)]);
		}
	if (hi[0] < x0 || lo[0] > x1 || hi[1] < y0 || lo[1] > y1) return false;
	const double corners[4][2] = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
	for (size_t i = 0; n >= 2 && i < n; ++i) {
		const size_t j = (i + 1) % n;
		const double nx = hull[2 * j + 1] - hull[2 * i + 1], ny = hull[2 * i] - hull[2 * j];
		double hull_lo = INFINITY, hull_hi = -INFINITY, box_lo = INFINITY, box_hi = -INFINITY;
		for (size_t k = 0; k < n; ++k) {
			const double d = hull[2 * k] * nx + hull[2 * k + 1] * ny;
			hull_lo = std::min(hull_lo, d);
			hull_hi = std::max(hull_hi, d);
		}
		for (const auto &c : corners) {
			const double d = c[0] * nx + c[1] * ny;
			box_lo = std::min(box_lo, d);
			box_hi = std::max(box_hi, d);
		}
		if (hull_hi < box_lo || box_hi < hull_lo) return false;
	}
	return true;
}

MissionMapFootprint mission_map_footprint(std::shared_ptr<const MissionModelOutline> outline, double x, double y,
		double pitch, double yaw, double roll, int32_t scale_q16) {
	MissionMapFootprint out;
	out.origin[0] = x;
	out.origin[1] = y;
	// The placement matrix's columns: each word's unit carried as mission_anchor_offset carries a point.
	for (int k = 0; k < 3; ++k) {
		double unit[3] = { 0.0, 0.0, 0.0 }, column[3];
		unit[k] = 1.0;
		mission_anchor_offset(unit, scale_q16, pitch, yaw, roll, column);
		out.basis[0][k] = column[0];
		out.basis[1][k] = column[1];
	}
	out.outline = std::move(outline);
	if (!out.outline) return out;
	std::vector<std::array<double, 2>> placed;
	if (pitch == 0.0 && roll == 0.0) {
		const std::vector<float> &hull = out.outline->hull;
		for (size_t i = 0; i + 1 < hull.size(); i += 2) {
			const float words[3] = { hull[i], hull[i + 1], 0.0f };
			double px, py;
			out.place(words, px, py);
			placed.push_back({ px, py });
		}
	} else {
		const std::vector<float> &points = out.outline->points;
		for (size_t i = 0; i + 2 < points.size(); i += 3) {
			double px, py;
			out.place(&points[i], px, py);
			placed.push_back({ px, py });
		}
	}
	out.hull = convex_hull(std::move(placed));
	const size_t n = out.hull.size() / 2;
	if (n == 0) return out;
	double lo[2] = { INFINITY, INFINITY }, hi[2] = { -INFINITY, -INFINITY };
	for (size_t i = 0; i < n; ++i)
		for (int axis = 0; axis < 2; ++axis) {
			lo[axis] = std::min(lo[axis], out.hull[2 * i + size_t(axis)]);
			hi[axis] = std::max(hi[axis], out.hull[2 * i + size_t(axis)]);
		}
	out.centre[0] = (lo[0] + hi[0]) * 0.5;
	out.centre[1] = (lo[1] + hi[1]) * 0.5;
	for (size_t i = 0; i < n; ++i)
		out.reach = std::max(out.reach, std::hypot(out.hull[2 * i] - out.centre[0], out.hull[2 * i + 1] - out.centre[1]));
	out.area = std::fabs(signed_area(out.hull));
	return out;
}

// --- the cache -------------------------------------------------------------------------------------

bool MissionOutlineCache::step(const SessionView &view, const std::vector<int64_t> &items, int64_t budget_us) {
	const uint64_t graph = view.findings.graph ? view.findings.graph->generation() : 0;
	const uint64_t files = view.findings.assets ? view.findings.assets->generation() : 0;
	// Another graph or other files: every item asked again, its outline standing meanwhile.
	if (!generations_ || graph != graph_generation_ || files != files_generation_) {
		generations_ = true;
		graph_generation_ = graph;
		files_generation_ = files;
		++epoch_;
	}
	const auto start = std::chrono::steady_clock::now();
	bool asked = false, changed = false;
	pending_ = 0;
	for (const int64_t id : items) {
		Item &item = items_[id];
		if (item.epoch == epoch_) continue;
		if (asked && std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count() >=
				budget_us) {
			++pending_;
			continue;
		}
		MissionItemFacts facts;
		std::string ignored;
		cache_.facts(view, id, facts, ignored);
		asked = true;
		item.epoch = epoch_;
		if (item.outline != facts.outline || item.scale_q16 != facts.scale_q16) {
			item.outline = facts.outline;
			item.scale_q16 = facts.scale_q16;
			changed = true;
		}
	}
	return changed;
}

const MissionOutlineCache::Item *MissionOutlineCache::item(int64_t id) const {
	const auto found = items_.find(id);
	return found == items_.end() ? nullptr : &found->second;
}

} // namespace opennova::editor
