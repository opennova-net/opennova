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

// --- RGBA8 colour / blend brushes ---

float rgba8_to_float(uint8_t b) {
	// Godot FORMAT_RGBA8 get_pixel: Color component = float(byte / 255.0).
	return static_cast<float>(b / 255.0);
}

uint8_t float_to_rgba8(float c) {
	// Godot FORMAT_RGBA8 set_pixel: byte = uint8_t(CLAMP(component * 255.0, 0, 255)),
	// where the uint8_t cast truncates toward zero (verified empirically).
	double v = static_cast<double>(c) * 255.0;
	if (v < 0.0) {
		v = 0.0;
	} else if (v > 255.0) {
		v = 255.0;
	}
	return static_cast<uint8_t>(v);
}

bool brush_blend_paint(uint8_t *buf, int width, int height, int channel, int cx, int cz, int radius,
                       double strength, double hardness, const BrushRect &clip) {
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
			const size_t ofs = (static_cast<size_t>(z) * width + x) * 4;
			// Read RGB into double (matches `var red := current.r`: f32 -> double).
			double red = static_cast<double>(rgba8_to_float(buf[ofs + 0]));
			double green = static_cast<double>(rgba8_to_float(buf[ofs + 1]));
			double blue = static_cast<double>(rgba8_to_float(buf[ofs + 2]));
			const double weighted = falloff * falloff;
			const double amount = strength * weighted;
			if (channel == 0) {
				red += amount;
			} else if (channel == 1) {
				green += amount;
			} else {
				blue += amount;
			}
			const double total = red + green + blue;
			if (total > 0.001) {
				red /= total;
				green /= total;
				blue /= total;
			} else {
				red = (channel == 0) ? 1.0 : 0.0;
				green = (channel == 1) ? 1.0 : 0.0;
				blue = (channel == 2) ? 1.0 : 0.0;
			}
			// Narrow double -> f32 (Color ctor) then encode (set_pixel). Alpha -> 1.0.
			buf[ofs + 0] = float_to_rgba8(static_cast<float>(red));
			buf[ofs + 1] = float_to_rgba8(static_cast<float>(green));
			buf[ofs + 2] = float_to_rgba8(static_cast<float>(blue));
			buf[ofs + 3] = float_to_rgba8(1.0f);
			touched = true;
		}
	}
	return touched;
}

// Float32 RGBA lerp matching Godot Color::lerp (Math::lerp per component:
// from + (to - from) * weight).
namespace {
inline void lerp_rgba_f32(const uint8_t *src_px, float to_r, float to_g, float to_b, float to_a,
                          float weight, uint8_t *out_px) {
	const float cr = rgba8_to_float(src_px[0]);
	const float cg = rgba8_to_float(src_px[1]);
	const float cb = rgba8_to_float(src_px[2]);
	const float ca = rgba8_to_float(src_px[3]);
	out_px[0] = float_to_rgba8(cr + (to_r - cr) * weight);
	out_px[1] = float_to_rgba8(cg + (to_g - cg) * weight);
	out_px[2] = float_to_rgba8(cb + (to_b - cb) * weight);
	out_px[3] = float_to_rgba8(ca + (to_a - ca) * weight);
}
} // namespace

bool brush_colormap_paint(uint8_t *buf, int width, int height,
                          float color_r, float color_g, float color_b, float color_a,
                          int cx, int cz, int radius, double strength, double hardness, const BrushRect &clip) {
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
			const float t = static_cast<float>(clamp01(strength * falloff * falloff));
			uint8_t *px = &buf[(static_cast<size_t>(z) * width + x) * 4];
			lerp_rgba_f32(px, color_r, color_g, color_b, color_a, t, px);
			touched = true;
		}
	}
	return touched;
}

bool brush_colormap_clone(uint8_t *dest, int width, int height, const uint8_t *src, int src_w, int src_h,
                          int src_cx, int src_cy, int dst_cx, int dst_cy, int radius,
                          double strength, double hardness, const BrushRect &clip) {
	const BrushWindow w = make_window(width, height, dst_cx, dst_cy, radius, hardness, clip);
	if (w.empty || src_w <= 0 || src_h <= 0) {
		return false;
	}
	bool touched = false;
	for (int z = w.z_min; z < w.z_max; ++z) {
		for (int x = w.x_min; x < w.x_max; ++x) {
			const double falloff = pixel_falloff(w, x, z, dst_cx, dst_cy);
			if (falloff < 0.0) {
				continue;
			}
			int sx = x - dst_cx + src_cx;
			int sy = z - dst_cy + src_cy;
			sx = sx < 0 ? 0 : (sx > src_w - 1 ? src_w - 1 : sx);
			sy = sy < 0 ? 0 : (sy > src_h - 1 ? src_h - 1 : sy);
			const uint8_t *s = &src[(static_cast<size_t>(sy) * src_w + sx) * 4];
			const float t = static_cast<float>(clamp01(strength * falloff * falloff));
			uint8_t *px = &dest[(static_cast<size_t>(z) * width + x) * 4];
			lerp_rgba_f32(px, rgba8_to_float(s[0]), rgba8_to_float(s[1]), rgba8_to_float(s[2]),
			              rgba8_to_float(s[3]), t, px);
			touched = true;
		}
	}
	return touched;
}

void brush_sample_color(const uint8_t *buf, int width, int height, double world_x, double world_z,
                        uint8_t out_rgba[4]) {
	out_rgba[0] = out_rgba[1] = out_rgba[2] = 0;
	out_rgba[3] = 255;
	if (width <= 0 || height <= 0) {
		return;
	}
	int sx = static_cast<int>(world_x);
	int sz = static_cast<int>(world_z);
	sx = sx < 0 ? 0 : (sx > width - 1 ? width - 1 : sx);
	sz = sz < 0 ? 0 : (sz > height - 1 ? height - 1 : sz);
	const uint8_t *px = &buf[(static_cast<size_t>(sz) * width + sx) * 4];
	out_rgba[0] = px[0];
	out_rgba[1] = px[1];
	out_rgba[2] = px[2];
	out_rgba[3] = px[3];
}

} // namespace opennova::terrain
