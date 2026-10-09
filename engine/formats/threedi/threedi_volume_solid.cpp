// The convex solid a collision volume bounds (threedi_volume_solid.h): inside
// is n . p + d <= 0 for every plane, the game's contained-point test
// [orig: Entity_ComputeBoneCollisionForce @ 0x4ae150].

#include <formats/threedi/threedi_volume_solid.h>

#include <base/io/fixed.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace opennova::threedi {

namespace {

constexpr double q16(int32_t v) { return v / io::kFp16OneD; }

} // namespace

std::vector<std::vector<ThreediBuildVec3>> threedi_volume_polygons(const ThreediBoundingPlane *planes,
                                                                   size_t count, bool ladder) {
	std::vector<std::vector<ThreediBuildVec3>> out;
	if (!planes || count < 3) return out;
	struct Plane {
		double n[3];
		double d;
	};
	std::vector<Plane> p(count);
	for (size_t i = 0; i < count; ++i) p[i] = Plane{{planes[i].normal[0], planes[i].normal[1], planes[i].normal[2]}, planes[i].radius};
	// Every point three planes meet in that lies within 1 mm of the solid (retail's stored planes meet a
	// few tenths of a millimetre off where four or more should).
	struct Corner {
		ThreediBuildVec3 at;
		size_t planes[3];
		double excess;
	};
	std::vector<Corner> corners;
	for (size_t i = 0; i < count; ++i)
		for (size_t j = i + 1; j < count; ++j)
			for (size_t k = j + 1; k < count; ++k) {
				const Plane &a = p[i], &b = p[j], &c = p[k];
				const double det = a.n[0] * (b.n[1] * c.n[2] - b.n[2] * c.n[1]) - a.n[1] * (b.n[0] * c.n[2] - b.n[2] * c.n[0]) +
				                   a.n[2] * (b.n[0] * c.n[1] - b.n[1] * c.n[0]);
				if (std::fabs(det) < 1e-9) continue;
				const double r[3] = {-a.d, -b.d, -c.d};
				const double x = (r[0] * (b.n[1] * c.n[2] - b.n[2] * c.n[1]) - a.n[1] * (r[1] * c.n[2] - b.n[2] * r[2]) +
				                  a.n[2] * (r[1] * c.n[1] - b.n[1] * r[2])) / det;
				const double y = (a.n[0] * (r[1] * c.n[2] - b.n[2] * r[2]) - r[0] * (b.n[0] * c.n[2] - b.n[2] * c.n[0]) +
				                  a.n[2] * (b.n[0] * r[2] - r[1] * c.n[0])) / det;
				const double z = (a.n[0] * (b.n[1] * r[2] - r[1] * c.n[1]) - a.n[1] * (b.n[0] * r[2] - r[1] * c.n[0]) +
				                  r[0] * (b.n[0] * c.n[1] - b.n[1] * c.n[0])) / det;
				double excess = -1e30;
				for (const Plane &q : p) excess = std::max(excess, q.n[0] * x + q.n[1] * y + q.n[2] * z + q.d);
				if (excess <= 1e-3) corners.push_back(Corner{ThreediBuildVec3{x, y, z}, {i, j, k}, excess});
			}
	const auto distance2 = [](const ThreediBuildVec3 &a, const ThreediBuildVec3 &b) {
		return (a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z);
	};
	std::vector<std::pair<size_t, std::vector<ThreediBuildVec3>>> facets;
	for (size_t pi = 0; pi < count; ++pi) {
		// The corners on this plane, each once, wound about its normal and each sub-millimetre cluster
		// collapsed to the corner deepest inside.
		std::vector<std::pair<ThreediBuildVec3, double>> on;
		for (const Corner &corner : corners) {
			if (corner.planes[0] != pi && corner.planes[1] != pi && corner.planes[2] != pi) continue;
			bool seen = false;
			for (const auto &o : on) seen = seen || distance2(o.first, corner.at) < 1e-12;
			if (!seen) on.push_back({corner.at, corner.excess});
		}
		if (on.size() < 3) continue;
		ThreediBuildVec3 middle;
		for (const auto &o : on) middle = ThreediBuildVec3{middle.x + o.first.x / on.size(), middle.y + o.first.y / on.size(), middle.z + o.first.z / on.size()};
		ThreediBuildVec3 u;
		double far = -1.0;
		for (const auto &o : on) {
			const ThreediBuildVec3 e{o.first.x - middle.x, o.first.y - middle.y, o.first.z - middle.z};
			const double l = e.x * e.x + e.y * e.y + e.z * e.z;
			if (l > far) {
				far = l;
				u = e;
			}
		}
		const double ul = std::sqrt(far);
		if (ul <= 1e-9) continue;
		u = ThreediBuildVec3{u.x / ul, u.y / ul, u.z / ul};
		const double *n = p[pi].n;
		const ThreediBuildVec3 w{n[1] * u.z - n[2] * u.y, n[2] * u.x - n[0] * u.z, n[0] * u.y - n[1] * u.x};
		std::sort(on.begin(), on.end(), [&](const auto &a, const auto &b) {
			const auto angle = [&](const ThreediBuildVec3 &q) {
				const ThreediBuildVec3 e{q.x - middle.x, q.y - middle.y, q.z - middle.z};
				return std::atan2(e.x * w.x + e.y * w.y + e.z * w.z, e.x * u.x + e.y * u.y + e.z * u.z);
			};
			return angle(a.first) < angle(b.first);
		});
		std::vector<std::vector<std::pair<ThreediBuildVec3, double>>> clusters;
		for (const auto &o : on) {
			if (!clusters.empty() && distance2(o.first, clusters.back().back().first) < 1e-6) clusters.back().push_back(o);
			else clusters.push_back({o});
		}
		if (clusters.size() > 1 && distance2(clusters.front().front().first, clusters.back().back().first) < 1e-6) {
			std::vector<std::pair<ThreediBuildVec3, double>> merged = clusters.back();
			merged.insert(merged.end(), clusters.front().begin(), clusters.front().end());
			clusters.front() = std::move(merged);
			clusters.pop_back();
		}
		std::vector<ThreediBuildVec3> polygon;
		for (const auto &cluster : clusters)
			polygon.push_back(std::min_element(cluster.begin(), cluster.end(),
			                                   [](const auto &a, const auto &b) { return a.second < b.second; })
			                          ->first);
		double area = 0.0;
		for (size_t a = 1; a + 1 < polygon.size(); ++a) {
			const ThreediBuildVec3 e1{polygon[a].x - polygon[0].x, polygon[a].y - polygon[0].y, polygon[a].z - polygon[0].z};
			const ThreediBuildVec3 e2{polygon[a + 1].x - polygon[0].x, polygon[a + 1].y - polygon[0].y,
			                          polygon[a + 1].z - polygon[0].z};
			const ThreediBuildVec3 cr{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
			area += 0.5 * std::sqrt(cr.x * cr.x + cr.y * cr.y + cr.z * cr.z);
		}
		if (polygon.size() >= 3 && area > 1e-8) facets.push_back({pi, std::move(polygon)});
	}
	// A solid has four facets or more; a flat ladder keeps its facing's (plane 0's) alone.
	if (facets.size() < 4) {
		if (ladder)
			for (auto &facet : facets)
				if (facet.first == 0) out.push_back(std::move(facet.second));
		return out;
	}
	for (auto &facet : facets) out.push_back(std::move(facet.second));
	return out;
}

std::vector<std::vector<ThreediBuildVec3>> threedi_volume_solid(const ThreediBoundingVolume &volume,
                                                                const ThreediBoundingPlane *planes) {
	const size_t count = volume.plane_count > 0 ? size_t(volume.plane_count) : 0;
	// Type 4 is the ladder (CL), whose flat volume keeps its facing.
	const bool ladder = volume.collidable_type == 4;
	std::vector<std::vector<ThreediBuildVec3>> own = threedi_volume_polygons(planes, count, ladder);
	const double lo[3] = {q16(volume.min_x_fp16), q16(volume.min_y_fp16), q16(volume.min_z_fp16)};
	const double hi[3] = {q16(volume.max_x_fp16), q16(volume.max_y_fp16), q16(volume.max_z_fp16)};
	bool past = false;
	for (const auto &polygon : own)
		for (const ThreediBuildVec3 &p : polygon) {
			const double at[3] = {p.x, p.y, p.z};
			for (int k = 0; k < 3; ++k) past = past || at[k] < lo[k] - 0.002 || at[k] > hi[k] + 0.002;
		}
	if (!past || !(lo[0] <= hi[0] && lo[1] <= hi[1] && lo[2] <= hi[2])) return own;
	// The planes and the box's six (n . p + d <= 0 inside, as the volume's own).
	std::vector<ThreediBoundingPlane> clipped(planes, planes + count);
	for (int k = 0; k < 3; ++k)
		for (const int side : {1, -1}) {
			ThreediBoundingPlane plane{};
			plane.normal[k] = float(side);
			plane.radius = float(side > 0 ? -hi[k] : lo[k]);
			clipped.push_back(plane);
		}
	return threedi_volume_polygons(clipped.data(), clipped.size(), ladder);
}

} // namespace opennova::threedi
