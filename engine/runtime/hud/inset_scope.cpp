// Scoped + Inset terrain-window geometry [orig: Render_WeaponInsetScene @0x5C9740].
#include <algorithm>
#include <base/io/bam.h>
#include <cmath>
#include <runtime/hud/hud_math.h>
#include <runtime/hud/inset_scope.h>
#include <runtime/renderer/aspect_ratio.h>
namespace opennova::hud {
InsetScopeGeometry inset_scope_geometry(float w, float h, int aspect_mode, float fov) {
	InsetScopeGeometry g;
	if (w <= 0 || h <= 0 || fov <= 0)
		return g;
	const float ry = 160 * w / 1024, rx = ry * renderer::aspect_viewport_scale_y(aspect_mode, w, h);
	const int right = int(scale_axis(960, w, 1024)), top = int(scale_axis(224, h, 768));
	const int left = int(right - 2 * rx), bottom = int(top + 2 * ry);
	const int cx = (left + right) >> 1, cy = (top + bottom) >> 1;
	g.left = left - 1;
	g.right = right + 1;
	g.top = top - 1;
	g.bottom = bottom + 1;
	g.center_x = cx;
	g.center_y = cy;
	g.radius_x = rx;
	g.radius_y = ry;
	g.stroke = std::max(2 * w / 1024, 1.0f);
	g.cross_dx = (g.right - g.left) >> 3;
	g.cross_dy = (g.bottom - g.top) >> 3;
	g.fov_h_deg = ry * 0.0017578125f * fov;
	for (int i = 0; i <= 32; ++i) {
		const double a = (i * 32 % 1024) * (2 * io::kPi / 1024);
		const float c = float(int32_t(std::cos(a) * 4194304.0) / 4194304.0);
		const float s = float(int32_t(std::sin(a) * 4194304.0) / 4194304.0);
		g.inner[i] = { cx + rx * c, cy + ry * s };
		g.outer[i] = { (left + right) * 0.5f + 2 * rx * c, (top + bottom) * 0.5f + 2 * ry * s };
		g.uv[i] = { (g.inner[i].x - g.left) / (g.right - g.left + 1),
			(g.inner[i].y - g.top) / (g.bottom - g.top + 1) };
	}
	g.valid = g.right > g.left && g.bottom > g.top;
	return g;
}
} // namespace opennova::hud
