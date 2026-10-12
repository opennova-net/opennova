#include <editor/session/terrain_stroke_planes.h>
#include <editor/session/terrain_heights_plan.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <utility>

#include <editor/documents/terrain_heights_brush.h>
#include <editor/documents/terrain_heights_document.h>
#include <editor/preview/terrain_brush.h>
#include <editor/project/project_files.h>
#include <editor/session/session_core.h>
#include <editor/session/terrain_sculpt.h>
#include <runtime/terrain_query/atlas_routing.h>

namespace opennova::editor {

namespace {

// The routing every lookup of the game takes from a mission position to the terrain's atlas (the .trn's sector grid,
// its origin and wraps) [orig: Terrain_SampleHeightBilinear @ 0x6067BD..0x6068C4, through terrain_atlas_texel_at].
terrain::TerrainAtlasRouting routing_of(const TrnConfig &trn) {
	terrain::TerrainAtlasRouting routing;
	routing.sector_grid = &trn.sector_grid[0][0];
	routing.origin_x = trn.origin_x;
	routing.origin_y = trn.origin_y;
	routing.wrap_x = trn.wrap_x != 0;
	routing.wrap_z = trn.wrap_y != 0;
	return routing;
}

// The atlas texel a whole mission unit routes to (16.16 positions reach -32768 .. 32767 units).
terrain::TerrainAtlasTexel texel_at(const terrain::TerrainAtlasRouting &routing, int64_t x, int64_t y) {
	const auto fixed = [](int64_t unit) { return int32_t(std::clamp<int64_t>(unit, -32767, 32767) * 65536); };
	return terrain::terrain_atlas_texel_at(routing, fixed(x), fixed(y));
}

} // namespace

std::vector<HeightsDabTexel> heights_dab_texels(const TrnConfig &trn, const TerrainBrush &brush, TerrainPoint at) {
	const terrain::TerrainAtlasRouting routing = routing_of(trn);
	std::map<std::pair<int32_t, int32_t>, float> strongest;
	const double radius = std::max(0.0, double(brush.radius));
	const auto take = [&](int64_t x, int64_t y, float weight) {
		if (weight <= 0.0f) return;
		const terrain::TerrainAtlasTexel t = texel_at(routing, x, y);
		if (!t.mapped) return;
		float &w = strongest[{t.z, t.x}];
		w = std::max(w, weight);
	};
	if (radius <= 0.0) {
		take(std::llround(at.x), std::llround(at.y), 1.0f);
	} else {
		for (int64_t y = int64_t(std::ceil(at.y - radius)); y <= int64_t(std::floor(at.y + radius)); ++y)
			for (int64_t x = int64_t(std::ceil(at.x - radius)); x <= int64_t(std::floor(at.x + radius)); ++x)
				take(x, y, terrain_brush_weight(brush, std::hypot(double(x) - at.x, double(y) - at.y), false));
	}
	std::vector<HeightsDabTexel> out;
	out.reserve(strongest.size());
	for (const auto &[zx, weight] : strongest) out.push_back({zx.second, zx.first, weight});
	return out;
}

namespace {

const char *operator_words(HeightsOperator op) {
	switch (op) {
	case HeightsOperator::Raise: return "Raised";
	case HeightsOperator::Lower: return "Lowered";
	case HeightsOperator::Smooth: return "Smoothed";
	case HeightsOperator::Flatten: return "Flattened";
	case HeightsOperator::Set: return "Set";
	}
	return "Sculpted";
}

bool operator_of(TerrainTool tool, HeightsOperator &out) {
	switch (tool) {
	case TerrainTool::Raise: out = HeightsOperator::Raise; return true;
	case TerrainTool::Lower: out = HeightsOperator::Lower; return true;
	case TerrainTool::Smooth: out = HeightsOperator::Smooth; return true;
	case TerrainTool::Flatten: out = HeightsOperator::Flatten; return true;
	case TerrainTool::Set: out = HeightsOperator::Set; return true;
	default: return false;
	}
}

} // namespace

bool plan_heights_stroke(TerrainStrokeContext &context, TerrainStrokePlan &plan, std::string &error) {
	auto *heights = dynamic_cast<TerrainHeightsDocument *>(&context.target);
	if (!heights || heights->plane_count() < 2) {
		error = basename_of(context.target.path()) + " holds no heights the editor reads.";
		return false;
	}
	if (heights->blocked()) {
		error = basename_of(context.target.path()) + " is open read only: " +
		        (heights->issues().empty() ? std::string("its heights do not read") : heights->issues().front().message);
		return false;
	}
	if (!context.family.trn) {
		error = "The terrain's configuration does not read: " + basename_of(heights->path()) + " has no routing to sculpt by.";
		return false;
	}
	auto stroke = std::make_shared<HeightsStrokeEdit>();
	if (!operator_of(context.brush.tool, stroke->op)) {
		error = std::string("The ") + terrain_tool_label(context.brush.tool) + " tool does not sculpt.";
		return false;
	}
	TerrainSculpt &sculpt = context.core.sculpt();
	// A new stroke on the file stops a bake running for it (its regions join the next), and opens its ground's window.
	if (context.first) {
		sculpt.interrupt(heights->path());
		sculpt.stroke_began(*heights);
	}
	const terrain::TerrainAtlasRouting routing = routing_of(*context.family.trn);
	stroke->strength = std::clamp(context.brush.strength, 0.0f, 1.0f);
	// A PNG heightmap's white holds its heights (its 16-bit samples span 0 to it); else the word's top.
	stroke->top = heights->height_cap();
	for (const auto &[x, y] : context.points) stroke->dabs.push_back(heights_dab_texels(*context.family.trn, context.brush, {x, y}));
	if (stroke->op == HeightsOperator::Set) {
		stroke->target = int32_t(std::clamp(std::lround(double(context.brush.height) * 256.0), 0L, long(stroke->top)));
	} else if (stroke->op == HeightsOperator::Flatten) {
		// Towards the live height under the stroke's first sample (off the terrain, under the strongest texel its disc
		// reaches), kept for the gesture; a first sample reaching no ground sets none (the stroke is off the terrain).
		int32_t target = 0;
		if (context.first || !sculpt.flatten_target(context.gesture, target)) {
			const auto &[x, y] = context.points.front();
			const terrain::TerrainAtlasTexel t = texel_at(routing, std::llround(x), std::llround(y));
			const HeightsDabTexel *strongest = nullptr;
			if (!t.mapped)
				for (const HeightsDabTexel &texel : stroke->dabs.front())
					if (!strongest || texel.weight > strongest->weight) strongest = &texel;
			if (!t.mapped && !strongest) {
				error = "The brush is off the terrain there: no ground of " + basename_of(heights->path()) + " lies under it.";
				return false;
			}
			target = t.mapped ? heights->live_height(t.x, t.z) : heights->live_height(strongest->x, strongest->z);
			sculpt.remember_flatten(context.gesture, target);
		}
		stroke->target = target;
	}
	std::vector<HeightsDabTexel> touched;
	for (const std::vector<HeightsDabTexel> &dab : stroke->dabs) touched.insert(touched.end(), dab.begin(), dab.end());
	if (touched.empty()) {
		error = "The brush is off the terrain there: no ground of " + basename_of(heights->path()) + " lies under it.";
		return false;
	}
	for (const HeightsRect &rect : heights_regions(touched))
		plan.changed.push_back({heights->path(), TerrainPlane::Heights, rect.x, rect.y, rect.x + rect.w, rect.y + rect.h});
	// Ground the sector grid repeats: said so (every repeat shows the change).
	std::set<int> slots;
	int repeats = 0;
	{
		const auto &[x, y] = context.points.front();
		const terrain::TerrainAtlasTexel t = texel_at(routing, std::llround(x), std::llround(y));
		if (t.mapped)
			for (int cell = 0; cell < 256; ++cell) repeats += routing.sector_grid[cell] - 1 == t.slot;
	}
	plan.words = std::string(operator_words(stroke->op)) + " the ground in " + basename_of(heights->path()) +
	             (repeats > 1 ? " (this ground repeats in " + std::to_string(repeats) + " sectors)" : std::string());
	// Ground the heightmap's white holds down: said so, with the way up.
	bool capped = false;
	if (stroke->top < 65535) {
		if (stroke->op == HeightsOperator::Set) {
			capped = std::lround(double(context.brush.height) * 256.0) > stroke->top;
		} else if (stroke->op == HeightsOperator::Raise) {
			for (const HeightsDabTexel &t : touched)
				capped = capped || double(heights->live_height(t.x, t.z)) + double(stroke->strength) * 256.0 * t.weight >
				                           double(stroke->top);
		}
	}
	if (capped) {
		char top[32];
		std::snprintf(top, sizeof(top), "%g", stroke->top / 256.0);
		plan.words += std::string(" (held at the heightmap's white, ") + top + " units: raise the import's top first)";
	}
	plan.bake = "Baking " + basename_of(heights->path()) + " as the stroke ends";
	plan.edits.push_back(RasterDocument::edit(stroke, context.gesture));
	return true;
}

} // namespace opennova::editor
