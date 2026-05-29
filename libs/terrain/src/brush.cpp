#include "terrain/brush.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace opennova::terrain {

namespace {

constexpr double BRUSH_HEIGHT_MAX = 255.996; // matches the GDScript clampf ceiling

inline double clamp01(double v) {
	return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

// The clipped iteration window plus the falloff parameters, mirroring
// _for_each_brush_pixel's setup.
struct BrushWindow {
	int x_min = 0;
	int x_max = 0;
	int z_min = 0;
	int z_max = 0;
	double radius_f = 0.0;
	double radius_sq = 0.0;
	double shoulder = 0.0;
	bool empty = true;
};

BrushWindow make_window(int width, int height, int cx, int cz, int radius, double hardness,
                        const BrushRect &clip) {
	BrushWindow w;
	int x_min = std::max(cx - radius, 0);
	int x_max = std::min(cx + radius + 1, width);
	int z_min = std::max(cz - radius, 0);
	int z_max = std::min(cz + radius + 1, height);

	if (clip.w > 0 && clip.h > 0) {
		x_min = std::max(x_min, clip.x);
		z_min = std::max(z_min, clip.z);
		x_max = std::min(x_max, clip.x + clip.w);
		z_max = std::min(z_max, clip.z + clip.h);
	}

	w.x_min = x_min;
	w.x_max = x_max;
	w.z_min = z_min;
	w.z_max = z_max;
	w.radius_f = static_cast<double>(radius);
	w.radius_sq = w.radius_f * w.radius_f;
	const double core = clamp01(hardness);
	w.shoulder = 1.0 - core;
	w.empty = (x_max <= x_min || z_max <= z_min || w.radius_f <= 0.0);
	return w;
}

// falloff at a pixel given its distance^2 from the center, or a negative value
// when the pixel is outside the disc (caller skips it).
inline double pixel_falloff(const BrushWindow &w, int x, int z, int cx, int cz) {
	const double dx = static_cast<double>(x - cx);
	const double dz = static_cast<double>(z - cz);
	const double distance_sq = dx * dx + dz * dz;
	if (distance_sq > w.radius_sq) {
		return -1.0;
	}
	const double t_edge = 1.0 - std::sqrt(distance_sq) / w.radius_f;
	return brush_falloff(t_edge, w.shoulder);
}

} // namespace

double brush_falloff(double t_edge, double shoulder) {
	if (shoulder <= 0.0001) {
		return 1.0;
	}
	if (t_edge >= shoulder) {
		return 1.0;
	}
	const double u = t_edge / shoulder;
	return u * u * (3.0 - 2.0 * u);
}

bool brush_raise_lower(float *buf, int width, int height, int cx, int cz, int radius,
                       double amount, double hardness, const BrushRect &clip) {
	const BrushWindow w = make_window(width, height, cx, cz, radius, hardness, clip);
	if (w.empty) {
		return false;
	}
	bool touched = false;
	for (int z = w.z_min; z < w.z_max; ++z) {
		for (int x = w.x_min; x < w.x_max; ++x) {
			const double falloff = pixel_falloff(w, x, z, cx, cz);
			if (falloff < 0.0) {
				continue;
			}
			const double weighted = falloff * falloff;
			double h = static_cast<double>(buf[static_cast<size_t>(z) * width + x]);
			h = h + amount * weighted;
			h = h < 0.0 ? 0.0 : (h > BRUSH_HEIGHT_MAX ? BRUSH_HEIGHT_MAX : h);
			buf[static_cast<size_t>(z) * width + x] = static_cast<float>(h);
			touched = true;
		}
	}
	return touched;
}

bool brush_flatten(float *buf, int width, int height, int cx, int cz, int radius,
                   double target_height, double strength, double hardness, const BrushRect &clip) {
	const BrushWindow w = make_window(width, height, cx, cz, radius, hardness, clip);
	if (w.empty) {
		return false;
	}
	bool touched = false;
	for (int z = w.z_min; z < w.z_max; ++z) {
		for (int x = w.x_min; x < w.x_max; ++x) {
			const double falloff = pixel_falloff(w, x, z, cx, cz);
			if (falloff < 0.0) {
				continue;
			}
			const double h = static_cast<double>(buf[static_cast<size_t>(z) * width + x]);
			const double t = clamp01(strength * falloff);
			// lerp(h, target, t) == h + (target - h) * t
			const double result = h + (target_height - h) * t;
			buf[static_cast<size_t>(z) * width + x] = static_cast<float>(result);
			touched = true;
		}
	}
	return touched;
}

bool brush_smooth(float *buf, int width, int height, int cx, int cz, int radius,
                  double strength, double hardness, const BrushRect &clip) {
	const BrushWindow w = make_window(width, height, cx, cz, radius, hardness, clip);
	if (w.empty) {
		return false;
	}
	// Pass 1: snapshot the whole window (the GDScript snapshot covers only the
	// disc, but window pixels outside the disc are never mutated, so a dense
	// window snapshot reads back identically to originals.get(key, live)).
	const int win_w = w.x_max - w.x_min;
	const int win_h = w.z_max - w.z_min;
	std::vector<float> snap(static_cast<size_t>(win_w) * static_cast<size_t>(win_h));
	for (int z = w.z_min; z < w.z_max; ++z) {
		for (int x = w.x_min; x < w.x_max; ++x) {
			snap[static_cast<size_t>(z - w.z_min) * win_w + (x - w.x_min)] =
			        buf[static_cast<size_t>(z) * width + x];
		}
	}

	// Pass 2: average a 3x3 window clamped to IMAGE bounds (not the clip rect).
	bool touched = false;
	for (int z = w.z_min; z < w.z_max; ++z) {
		for (int x = w.x_min; x < w.x_max; ++x) {
			const double falloff = pixel_falloff(w, x, z, cx, cz);
			if (falloff < 0.0) {
				continue;
			}
			double sum = 0.0;
			int count = 0;
			const int nz0 = std::max(z - 1, 0);
			const int nz1 = std::min(z + 2, height);
			const int nx0 = std::max(x - 1, 0);
			const int nx1 = std::min(x + 2, width);
			for (int nz = nz0; nz < nz1; ++nz) {
				for (int nx = nx0; nx < nx1; ++nx) {
					double v;
					if (nx >= w.x_min && nx < w.x_max && nz >= w.z_min && nz < w.z_max) {
						v = static_cast<double>(snap[static_cast<size_t>(nz - w.z_min) * win_w + (nx - w.x_min)]);
					} else {
						v = static_cast<double>(buf[static_cast<size_t>(nz) * width + nx]);
					}
					sum += v;
					count += 1;
				}
			}
			const double h = static_cast<double>(snap[static_cast<size_t>(z - w.z_min) * win_w + (x - w.x_min)]);
			const double t = clamp01(strength * falloff);
			const double avg = sum / static_cast<double>(count);
			const double result = h + (avg - h) * t; // lerp(h, avg, t)
			buf[static_cast<size_t>(z) * width + x] = static_cast<float>(result);
			touched = true;
		}
	}
	return touched;
}

} // namespace opennova::terrain
