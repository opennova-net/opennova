#include <editor/preview/terrain_brush.h>

#include <algorithm>
#include <cmath>

namespace opennova::editor {

float terrain_brush_weight(const TerrainBrush &brush, double distance, bool categorical) {
	const double radius = std::max(0.0, double(brush.radius));
	if (distance < 0.0) distance = -distance;
	if (radius <= 0.0) return distance <= 0.0 ? 1.0f : 0.0f;
	if (distance > radius) return 0.0f;
	if (categorical) return 1.0f;
	const double inner = radius * (1.0 - std::clamp(double(brush.falloff), 0.0, 1.0));
	if (distance <= inner) return 1.0f;
	const double t = (radius - distance) / (radius - inner); // 1 at the inner edge, 0 at the rim
	return float(t * t * (3.0 - 2.0 * t));
}

double terrain_dab_step(const TerrainBrush &brush, double texel) {
	return std::max(double(brush.spacing) * double(brush.radius), 0.5 * texel);
}

void TerrainDabPath::begin(TerrainPoint at, std::vector<TerrainPoint> &dabs) {
	started_ = true;
	last_ = at;
	walked_ = 0.0;
	dabs.push_back(at);
}

void TerrainDabPath::to(TerrainPoint at, double step, std::vector<TerrainPoint> &dabs) {
	if (!started_) return begin(at, dabs);
	const double dx = at.x - last_.x, dy = at.y - last_.y;
	const double length = std::sqrt(dx * dx + dy * dy);
	if (!(step > 0.0) || !(length > 0.0)) return;
	// The dabs every `step` metres from the last one, the walk carried on past them.
	double along = step - walked_;
	while (along <= length) {
		dabs.push_back(TerrainPoint{last_.x + dx * along / length, last_.y + dy * along / length});
		along += step;
	}
	walked_ = length - (along - step);
	last_ = at;
}

} // namespace opennova::editor
