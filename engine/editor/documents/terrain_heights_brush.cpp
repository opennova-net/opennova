#include "terrain_heights_brush.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <utility>

#include <editor/model/diagnostic.h>
#include <editor/model/finding_code_row.h>

namespace opennova::editor {

namespace {

constexpr int kCell = 64; // a leaf of the bake's quadtree, the region a bake remakes

int32_t at(const RasterPlane &plane, int32_t x, int32_t z) { return plane.u16(x & 1023, z & 1023); }

} // namespace

std::vector<HeightsRect> heights_regions(const std::vector<HeightsDabTexel> &texels) {
	// Each leaf cell's texels as their bounding rectangle.
	std::map<std::pair<int, int>, HeightsRect> cells;
	for (const HeightsDabTexel &t : texels) {
		const std::pair<int, int> key{t.z / kCell, t.x / kCell};
		auto found = cells.find(key);
		if (found == cells.end()) {
			cells.emplace(key, HeightsRect{t.x, t.z, 1, 1});
			continue;
		}
		HeightsRect &r = found->second;
		const int x1 = std::max(r.x + r.w, t.x + 1), z1 = std::max(r.y + r.h, t.z + 1);
		r.x = std::min(r.x, t.x);
		r.y = std::min(r.y, t.z);
		r.w = x1 - r.x;
		r.h = z1 - r.y;
	}
	std::vector<HeightsRect> out;
	for (const auto &entry : cells) out.push_back(entry.second);
	return out;
}

void join_heights_regions(std::vector<HeightsRect> &due, const std::vector<HeightsRect> &added) {
	// One rectangle a leaf cell: an added one joins its cell's.
	for (const HeightsRect &rect : added) {
		bool joined = false;
		for (HeightsRect &have : due) {
			if (have.x / kCell != rect.x / kCell || have.y / kCell != rect.y / kCell) continue;
			const int x1 = std::max(have.x + have.w, rect.x + rect.w), y1 = std::max(have.y + have.h, rect.y + rect.h);
			have.x = std::min(have.x, rect.x);
			have.y = std::min(have.y, rect.y);
			have.w = x1 - have.x;
			have.h = y1 - have.y;
			joined = true;
			break;
		}
		if (!joined) due.push_back(rect);
	}
}

bool HeightsStrokeEdit::apply(RasterCanvas &canvas, Diagnostic &error) const {
	const auto *state = dynamic_cast<const HeightsState *>(canvas.side());
	if (!state || canvas.plane_count() < 2 || canvas.plane(kHeightsInput).format() != RasterFormat::U16) {
		error = make_finding(CoreFinding::DocumentPayload, DiagnosticSeverity::Error,
		                     "A heights stroke edits a terrain's 16-bit heights.");
		return false;
	}
	RasterWriter &writer = canvas.write(kHeightsInput);
	const RasterPlane &plane = writer.plane();
	const double ceiling = double(std::clamp<int32_t>(top, 0, 65535));
	const auto settle = [ceiling](double h) { return uint16_t(std::clamp(std::floor(h + 0.5), 0.0, ceiling)); };
	std::vector<HeightsDabTexel> touched;
	std::vector<uint16_t> next;
	for (const std::vector<HeightsDabTexel> &dab : dabs) {
		next.resize(dab.size());
		// Every result of the dab from the heights as the dab found them, then written.
		for (size_t i = 0; i < dab.size(); ++i) {
			const HeightsDabTexel &t = dab[i];
			const double h = plane.u16(t.x, t.z);
			const double w = double(std::clamp(t.weight, 0.0f, 1.0f)) * double(std::clamp(strength, 0.0f, 1.0f));
			double out = h;
			switch (op) {
			case HeightsOperator::Raise: out = h + 256.0 * double(strength) * double(std::clamp(t.weight, 0.0f, 1.0f)); break;
			case HeightsOperator::Lower: out = h - 256.0 * double(strength) * double(std::clamp(t.weight, 0.0f, 1.0f)); break;
			case HeightsOperator::Smooth: {
				// TrnGen's kernel at the texel [orig: TrnGen.exe build_terrain_thread @ 0x4013A0] and its mirror.
				const int32_t x = t.x, z = t.z;
				const int32_t ahead = (at(plane, x, z) + at(plane, x + 1, z) + at(plane, x, z + 1) + at(plane, x + 1, z + 1)) >> 2;
				const int32_t behind = (at(plane, x, z) + at(plane, x - 1, z) + at(plane, x, z - 1) + at(plane, x - 1, z - 1)) >> 2;
				out = h + ((ahead + behind) * 0.5 - h) * w;
				break;
			}
			case HeightsOperator::Flatten:
			case HeightsOperator::Set: out = h + (double(target) - h) * w; break;
			}
			next[i] = settle(out);
		}
		for (size_t i = 0; i < dab.size(); ++i) writer.set_u16(dab[i].x, dab[i].z, next[i]);
		touched.insert(touched.end(), dab.begin(), dab.end());
	}
	if (touched.empty()) return true;
	auto after = std::make_shared<HeightsState>(*state);
	join_heights_regions(after->unbaked, heights_regions(touched));
	canvas.set_side(std::move(after));
	return true;
}

} // namespace opennova::editor
