#include <runtime/renderer/texture_dxt.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace opennova::renderer {
namespace {

// The codec's float constants, bit for bit.
constexpr float kInv31 = 0x1.084210p-5f;   // flt_7F6848
constexpr float kInv63 = 0x1.041042p-6f;   // flt_7DA070
constexpr float kInv255 = 0x1.010102p-8f;  // flt_7D75E8
constexpr float kThird = 0x1.555556p-2f;   // flt_7CA274
constexpr float kTwoThirds = 0x1.555556p-1f; // flt_7FB62C
constexpr float kFifth = 0x1.99999ap-3f;   // flt_7C3340
constexpr float kSeventh = 0x1.24924ap-3f; // flt_7F6868
constexpr float kFltMin = 0x1.0p-126f;     // flt_7E6330
constexpr float kEpsilonRgb = 0x1.0p-16f;  // flt_7FB5EC
constexpr float kMinLength = 0x1.0p-12f;   // flt_7FB5F0

// g_Luminance and its inverse. [orig: flt_85889C..flt_8588B8]
constexpr float kLuminanceR = 0x1.302a60p-2f;
constexpr float kLuminanceG = 1.0f;
constexpr float kLuminanceB = 0x1.9cce68p-4f;
constexpr float kInverseLuminanceR = 0x1.aeec5cp+1f;
constexpr float kInverseLuminanceG = 1.0f;
constexpr float kInverseLuminanceB = 0x1.3d83bap+3f;

// Endpoint weight tables. [orig: unk_7FB620/unk_7FB614 (three colors),
// unk_7FB604/unk_7FB5F4 (four colors), unk_7FB5D4/unk_7FB5BC (six alphas),
// unk_7FB59C/g_D3DXTexAlphaD8 (eight alphas)]. The eight-alpha D table's last entry
// is 8/7 (0x3F924925), not 1.
constexpr float kC3[] = {1.0f, 0.5f, 0.0f};
constexpr float kD3[] = {0.0f, 0.5f, 1.0f};
constexpr float kC4[] = {1.0f, kTwoThirds, kThird, 0.0f};
constexpr float kD4[] = {0.0f, kThird, kTwoThirds, 1.0f};
constexpr float kC6[] = {1.0f, 0x1.99999ap-1f, 0x1.333334p-1f, 0x1.99999ap-2f,
		0x1.99999ap-3f, 0.0f};
constexpr float kD6[] = {0.0f, 0x1.99999ap-3f, 0x1.99999ap-2f, 0x1.333334p-1f,
		0x1.99999ap-1f, 1.0f};
constexpr float kC8[] = {1.0f, 0x1.b6db6ep-1f, 0x1.6db6dcp-1f, 0x1.24924ap-1f,
		0x1.b6db6ep-2f, 0x1.24924ap-2f, 0x1.24924ap-3f, 0.0f};
constexpr float kD8[] = {0.0f, 0x1.24924ap-3f, 0x1.24924ap-2f, 0x1.b6db6ep-2f,
		0x1.24924ap-1f, 0x1.6db6dcp-1f, 0x1.b6db6ep-1f, 0x1.24924ap+0f};

// Palette position to block index. [orig: unk_7FB640, unk_7FB630,
// unk_7FB670, unk_7FB650]
constexpr uint32_t kSteps3[] = {0, 2, 1};
constexpr uint32_t kSteps4[] = {0, 2, 3, 1};
constexpr uint32_t kSteps6[] = {0, 2, 3, 4, 5, 1};
constexpr uint32_t kSteps8[] = {0, 2, 3, 4, 5, 6, 7, 1};

uint16_t read_u16(const uint8_t *bytes) {
	return static_cast<uint16_t>(bytes[0] | (bytes[1] << 8));
}

void write_u16(uint8_t *bytes, uint16_t value) {
	bytes[0] = static_cast<uint8_t>(value);
	bytes[1] = static_cast<uint8_t>(value >> 8);
}

// Floyd-Steinberg spread of one texel's error over the 4x4 block.
void diffuse_error(float *error, size_t stride, uint32_t index, float value) {
	if ((index & 3) != 3) {
		error[(index + 1) * stride] = value * 0.4375f + error[(index + 1) * stride];
	}
	if (index < 12) {
		if ((index & 3) != 0) {
			error[(index + 3) * stride] = value * 0.1875f + error[(index + 3) * stride];
		}
		error[(index + 4) * stride] = value * 0.3125f + error[(index + 4) * stride];
		if ((index & 3) != 3) {
			error[(index + 5) * stride] = value * 0.0625f + error[(index + 5) * stride];
		}
	}
}

// [orig: D3DXTex_EncodeR5G6B5 @ 0x72010D (clamp @ 0x720115..0x720196, round toward
// zero @ 0x720199..0x7201FB)]
uint16_t encode_565(float r, float g, float b) {
	if (r < 0.0f) r = 0.0f; else if (r > 1.0f) r = 1.0f;
	if (g < 0.0f) g = 0.0f; else if (g > 1.0f) g = 1.0f;
	if (b < 0.0f) b = 0.0f; else if (b > 1.0f) b = 1.0f;
	float scaled = r * 31.0f;
	const int32_t red = static_cast<int32_t>(scaled + 0.5f);
	scaled = g * 63.0f;
	const int32_t green = static_cast<int32_t>(scaled + 0.5f);
	scaled = b * 31.0f;
	const int32_t blue = static_cast<int32_t>(scaled + 0.5f);
	return static_cast<uint16_t>((((red << 6) | green) << 5) | blue);
}

// [orig: D3DXTex_UnpackR5G6B5ToFloat4 @ 0x72000B]
DxtColor decode_565(uint16_t packed) {
	DxtColor color;
	color.r = static_cast<float>(packed >> 11) * kInv31;
	color.g = static_cast<float>((packed >> 5) & 0x3F) * kInv63;
	color.b = static_cast<float>(packed & 0x1F) * kInv31;
	color.a = 1.0f;
	return color;
}

// Least-squares endpoint search over the luminance-weighted points.
// [orig: D3DXTex_OptimizeRGB @ 0x720537]
void optimize_rgb(DxtColor &out_x, DxtColor &out_y, const DxtColor *points,
		uint32_t steps) {
	const float *pc = steps == 3 ? kC3 : kC4;
	const float *pd = steps == 3 ? kD3 : kD4;

	// Bounding box: X starts at g_Luminance, Y at zero.
	// [orig: @ 0x720566..0x720600]
	float x_r = kLuminanceR;
	float x_g = kLuminanceG;
	float x_b = kLuminanceB;
	float y_r = 0.0f;
	float y_g = 0.0f;
	float y_b = 0.0f;
	for (uint32_t i = 0; i < 16; ++i) {
		const DxtColor &point = points[i];
		if (point.r < x_r) x_r = point.r;
		if (point.g < x_g) x_g = point.g;
		if (point.b < x_b) x_b = point.b;
		if (point.r > y_r) y_r = point.r;
		if (y_g < point.g) y_g = point.g;
		if (point.b > y_b) y_b = point.b;
	}

	// [orig: @ 0x720602..0x720663]
	const float ab_r = y_r - x_r;
	const float ab_g = y_g - x_g;
	const float ab_b = y_b - x_b;
	const float f_ab = (ab_b * ab_b + ab_g * ab_g) + ab_r * ab_r;
	if (f_ab < kFltMin) {
		out_x = {x_r, x_g, x_b, 0.0f};
		out_y = {y_r, y_g, y_b, 0.0f};
		return;
	}

	// Pick the box diagonal the points spread along most.
	// [orig: @ 0x720668..0x720773]
	const float inverse = 1.0f / f_ab;
	const float dir_r = ab_r * inverse;
	const float dir_g = ab_g * inverse;
	const float dir_b = inverse * ab_b;
	const float mid_r = (y_r + x_r) * 0.5f;
	const float mid_g = (y_g + x_g) * 0.5f;
	const float mid_b = (y_b + x_b) * 0.5f;
	float dir_error[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	for (uint32_t i = 0; i < 16; ++i) {
		const float pt_r = (points[i].r - mid_r) * dir_r;
		const float pt_g = (points[i].g - mid_g) * dir_g;
		const float pt_b = (points[i].b - mid_b) * dir_b;
		float f = (pt_g + pt_b) + pt_r;
		dir_error[0] = dir_error[0] + f * f;
		f = (pt_g + pt_r) - pt_b;
		dir_error[1] = f * f + dir_error[1];
		const float rg = pt_r - pt_g;
		f = rg + pt_b;
		dir_error[2] = f * f + dir_error[2];
		f = rg - pt_b;
		dir_error[3] = f * f + dir_error[3];
	}
	float dir_max = dir_error[0];
	uint32_t dir_index = 0;
	for (uint32_t d = 1; d < 4; ++d) {
		if (dir_max < dir_error[d]) {
			dir_index = d;
			dir_max = dir_error[d];
		}
	}
	if ((dir_index & 2) != 0) std::swap(x_g, y_g);
	if ((dir_index & 1) != 0) std::swap(x_b, y_b);
	if (f_ab < kMinLength) {
		out_x = {x_r, x_g, x_b, 0.0f};
		out_y = {y_r, y_g, y_b, 0.0f};
		return;
	}

	// Newton iterations on the sum of squared errors.
	// [orig: @ 0x7207AA..0x720A7B]
	const float f_steps = static_cast<float>(steps - 1);
	for (uint32_t iteration = 0; iteration < 8; ++iteration) {
		DxtColor step[4];
		for (uint32_t s = 0; s < steps; ++s) {
			step[s].r = x_r * pc[s] + y_r * pd[s];
			step[s].g = x_g * pc[s] + y_g * pd[s];
			step[s].b = x_b * pc[s] + y_b * pd[s];
		}
		float d_r = y_r - x_r;
		float d_g = y_g - x_g;
		float d_b = y_b - x_b;
		const float f_len = (d_b * d_b + d_g * d_g) + d_r * d_r;
		if (f_len < kMinLength) break;
		const float scale = f_steps / f_len;
		d_r = scale * d_r;
		d_g = d_g * scale;
		d_b = scale * d_b;

		float d2_x = 0.0f;
		float d2_y = 0.0f;
		float dx_r = 0.0f, dx_g = 0.0f, dx_b = 0.0f;
		float dy_r = 0.0f, dy_g = 0.0f, dy_b = 0.0f;
		for (uint32_t i = 0; i < 16; ++i) {
			const DxtColor &point = points[i];
			const float dot = ((point.g - x_g) * d_g + (point.r - x_r) * d_r) +
					(point.b - x_b) * d_b;
			// The step index has no lower guard: a point more than 1.5 steps
			// before X indexes the palette below its start, which reads the
			// stack under it. The first pass projects every point onto
			// [0, f_steps] (both endpoints are box corners), and no pass
			// reaches -1.5 on any shipped TGA (all of resource.pff,
			// localres.pff and RevX02.pff, both formats, every level); the
			// port holds such a point at index 0.
			// [orig: @ 0x7208CF..0x7208F5]
			int32_t s;
			if (dot < f_steps) {
				s = static_cast<int32_t>(dot + 0.5f);
				if (s < 0) s = 0;
			} else {
				s = static_cast<int32_t>(steps) - 1;
			}
			const float diff_r = step[s].r - point.r;
			const float diff_g = step[s].g - point.g;
			const float diff_b = step[s].b - point.b;
			const float fc = pc[s] * 0.125f;
			const float fd = pd[s] * 0.125f;
			d2_x = fc * pc[s] + d2_x;
			dx_r = fc * diff_r + dx_r;
			dx_g = fc * diff_g + dx_g;
			dx_b = fc * diff_b + dx_b;
			d2_y = fd * pd[s] + d2_y;
			dy_r = fd * diff_r + dy_r;
			dy_g = diff_g * fd + dy_g;
			dy_b = fd * diff_b + dy_b;
		}
		if (d2_x > 0.0f) {
			const float f = -1.0f / d2_x;
			x_r = dx_r * f + x_r;
			x_g = dx_g * f + x_g;
			x_b = f * dx_b + x_b;
		}
		if (d2_y > 0.0f) {
			const float f = -1.0f / d2_y;
			y_r = dy_r * f + y_r;
			y_g = dy_g * f + y_g;
			y_b = f * dy_b + y_b;
		}
		if (dx_r * dx_r < kEpsilonRgb && dx_g * dx_g < kEpsilonRgb &&
				dx_b * dx_b < kEpsilonRgb && dy_r * dy_r < kEpsilonRgb &&
				dy_g * dy_g < kEpsilonRgb && dy_b * dy_b < kEpsilonRgb) {
			break;
		}
	}
	out_x = {x_r, x_g, x_b, 0.0f};
	out_y = {y_r, y_g, y_b, 0.0f};
}

// The shared color block encoder; `colorkey` reserves index 3 of a
// three-color block for texels whose alpha is below 0.5.
// [orig: D3DXTex_EncodeDXT1Block @ 0x720AB7]
void encode_color_block(const DxtColor *texels, uint8_t *block, bool colorkey,
		bool dither) {
	// [orig: @ 0x720AC6..0x720B14]
	uint32_t steps = 4;
	if (colorkey) {
		uint32_t keyed = 0;
		for (uint32_t i = 0; i < 16; ++i) {
			if (texels[i].a < 0.5f) ++keyed;
		}
		if (keyed == 16) {
			write_u16(block, 0x0000);
			write_u16(block + 2, 0xFFFF);
			std::memset(block + 4, 0xFF, 4);
			return;
		}
		steps = keyed != 0 ? 3 : 4;
	}

	// Quantize every texel to 5:6:5 and weight it by luminance.
	// [orig: @ 0x720C09..0x720E45]
	DxtColor error[16];
	DxtColor color[16];
	for (uint32_t i = 0; i < 16; ++i) {
		float r = texels[i].r;
		float g = texels[i].g;
		float b = texels[i].b;
		if (dither) {
			r = r + error[i].r;
			g = g + error[i].g;
			b = b + error[i].b;
		}
		float scaled = r * 31.0f;
		const float quantized_r =
				static_cast<float>(static_cast<int32_t>(scaled + 0.5f)) * kInv31;
		scaled = g * 63.0f;
		const float quantized_g =
				static_cast<float>(static_cast<int32_t>(scaled + 0.5f)) * kInv63;
		scaled = b * 31.0f;
		const float quantized_b =
				static_cast<float>(static_cast<int32_t>(scaled + 0.5f)) * kInv31;
		color[i].a = 1.0f;
		if (dither) {
			diffuse_error(&error[0].r, 4, i, r - quantized_r);
			diffuse_error(&error[0].g, 4, i, g - quantized_g);
			diffuse_error(&error[0].b, 4, i, b - quantized_b);
		}
		color[i].r = quantized_r * kLuminanceR;
		color[i].g = quantized_g * kLuminanceG;
		color[i].b = quantized_b * kLuminanceB;
	}

	DxtColor endpoint_a;
	DxtColor endpoint_b;
	optimize_rgb(endpoint_a, endpoint_b, color, steps);

	// [orig: @ 0x720E65..0x720EEC]
	const uint16_t word_a = encode_565(endpoint_a.r * kInverseLuminanceR,
			endpoint_a.g * kInverseLuminanceG, endpoint_a.b * kInverseLuminanceB);
	const uint16_t word_b = encode_565(endpoint_b.r * kInverseLuminanceR,
			endpoint_b.g * kInverseLuminanceG, endpoint_b.b * kInverseLuminanceB);
	if (steps == 4 && word_a == word_b) {
		write_u16(block, word_a);
		write_u16(block + 2, word_b);
		std::memset(block + 4, 0, 4);
		return;
	}

	// [orig: @ 0x720EF1..0x720F9C]
	const DxtColor decoded_a = decode_565(word_a);
	const DxtColor decoded_b = decode_565(word_b);
	endpoint_a = {decoded_a.r * kLuminanceR, decoded_a.g * kLuminanceG,
			decoded_a.b * kLuminanceB, 0.0f};
	endpoint_b = {decoded_b.r * kLuminanceR, decoded_b.g * kLuminanceG,
			decoded_b.b * kLuminanceB, 0.0f};
	DxtColor step[4];
	if ((steps == 3) == (word_b >= word_a)) {
		write_u16(block, word_a);
		write_u16(block + 2, word_b);
		step[0] = endpoint_a;
		step[1] = endpoint_b;
	} else {
		write_u16(block, word_b);
		write_u16(block + 2, word_a);
		step[0] = endpoint_b;
		step[1] = endpoint_a;
	}

	// [orig: @ 0x720F9F..0x7210DE]
	const float d_r = step[1].r - step[0].r;
	const float d_g = step[1].g - step[0].g;
	const float d_b = step[1].b - step[0].b;
	const float d_a = step[1].a - step[0].a;
	const uint32_t *palette_index;
	if (steps == 3) {
		palette_index = kSteps3;
		step[2] = {0.5f * d_r + step[0].r, 0.5f * d_g + step[0].g,
				d_b * 0.5f + step[0].b, d_a * 0.5f + step[0].a};
	} else {
		palette_index = kSteps4;
		step[2] = {kThird * d_r + step[0].r, kThird * d_g + step[0].g,
				kThird * d_b + step[0].b, kThird * d_a + step[0].a};
		step[3] = {d_r * kTwoThirds + step[0].r, d_g * kTwoThirds + step[0].g,
				d_b * kTwoThirds + step[0].b, d_a * kTwoThirds + step[0].a};
	}

	// [orig: @ 0x7210E0..0x721141]
	const float f_steps = static_cast<float>(steps - 1);
	const float scale = word_a != word_b
			? f_steps / ((d_b * d_b + d_g * d_g) + d_r * d_r)
			: 0.0f;
	const float dir_r = d_r * scale;
	const float dir_g = scale * d_g;
	const float dir_b = scale * d_b;

	// [orig: @ 0x721145..0x7213FE]
	for (DxtColor &value : error) value = DxtColor{};
	uint32_t indices = 0;
	for (uint32_t i = 0; i < 16; ++i) {
		if (steps == 3 && texels[i].a < 0.5f) {
			indices = (indices >> 2) | 0xC0000000u;
			continue;
		}
		float r = kLuminanceR * texels[i].r;
		float g = kLuminanceG * texels[i].g;
		float b = kLuminanceB * texels[i].b;
		if (dither) {
			r = r + error[i].r;
			g = g + error[i].g;
			b = b + error[i].b;
		}
		const float dot = ((b - step[0].b) * dir_b + (g - step[0].g) * dir_g) +
				(r - step[0].r) * dir_r;
		uint32_t index;
		if (!(dot > 0.0f)) {
			index = 0;
		} else if (!(dot < f_steps)) {
			index = 1;
		} else {
			index = palette_index[static_cast<int32_t>(dot + 0.5f)];
		}
		indices = (index << 30) | (indices >> 2);
		if (dither) {
			diffuse_error(&error[0].r, 4, i, (r - step[index].r) * color[i].a);
			diffuse_error(&error[0].g, 4, i, (g - step[index].g) * color[i].a);
			diffuse_error(&error[0].b, 4, i, (b - step[index].b) * color[i].a);
		}
	}
	block[4] = static_cast<uint8_t>(indices);
	block[5] = static_cast<uint8_t>(indices >> 8);
	block[6] = static_cast<uint8_t>(indices >> 16);
	block[7] = static_cast<uint8_t>(indices >> 24);
}

// Newton iterations on the alpha endpoints; the step difference is taken as
// texel minus step.
// [orig: D3DXTex_OptimizeAlpha @ 0x720210]
void optimize_alpha(float &out_x, float &out_y, const float *points,
		uint32_t steps) {
	const float *pc = steps == 6 ? kC6 : kC8;
	const float *pd = steps == 6 ? kD6 : kD8;

	// [orig: @ 0x72023D..0x7202DB]
	float x = 1.0f;
	float y = 0.0f;
	if (steps == 8) {
		for (uint32_t i = 0; i < 16; ++i) {
			if (points[i] < x) x = points[i];
			if (points[i] > y) y = points[i];
		}
	} else {
		for (uint32_t i = 0; i < 16; ++i) {
			if (points[i] < x && points[i] > 0.0f) x = points[i];
			if (points[i] > y && points[i] < 1.0f) y = points[i];
		}
		if (x == y) y = 1.0f;
	}

	// [orig: @ 0x7202DE..0x7204C9]
	const float f_steps = static_cast<float>(steps - 1);
	for (uint32_t iteration = 0; iteration < 8; ++iteration) {
		const float range = y - x;
		if (range < 0.00390625f) break;
		const float scale = f_steps / range;
		float step[8];
		for (uint32_t s = 0; s < steps; ++s) {
			step[s] = y * pd[s] + x * pc[s];
		}
		if (steps == 6) {
			step[6] = 0.0f;
			step[7] = 1.0f;
		}
		float dx = 0.0f;
		float dy = 0.0f;
		float d2x = 0.0f;
		float d2y = 0.0f;
		for (uint32_t i = 0; i < 16; ++i) {
			const float point = points[i];
			const float dot = (point - x) * scale;
			uint32_t s;
			if (!(dot > 0.0f)) {
				if (steps == 6 && !(x * 0.5f < point)) continue;
				s = 0;
			} else if (!(dot < f_steps)) {
				if (steps == 6 && (y + 1.0f) * 0.5f <= point) continue;
				s = steps - 1;
			} else {
				s = static_cast<uint32_t>(static_cast<int32_t>(dot + 0.5f));
			}
			if (s >= steps) continue;
			const float diff = point - step[s];
			dx = diff * pc[s] + dx;
			d2x = pc[s] * pc[s] + d2x;
			dy = diff * pd[s] + dy;
			d2y = pd[s] * pd[s] + d2y;
		}
		if (d2x > 0.0f) x = x - dx / d2x;
		if (d2y > 0.0f) y = y - dy / d2y;
		if (x > y) std::swap(x, y);
		if (dx * dx < 0.015625f && dy * dy < 0.015625f) break;
	}

	// [orig: @ 0x7204D0..0x720531]
	out_x = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
	out_y = y < 0.0f ? 0.0f : (y > 1.0f ? 1.0f : y);
}

} // namespace

TextureDxtFormat select_texture_dxt_format(
		uint32_t creation_flags, const TextureDxtCaps &caps) {
	TextureDxtFormat format = TextureDxtFormat::None;
	if ((creation_flags & 0x300u) != 0) {
		if (caps.dxt5) format = TextureDxtFormat::Dxt5;
		if ((creation_flags & 0x400000u) != 0) {
			if ((creation_flags & 0x200u) == 0 && caps.dxt1) {
				format = TextureDxtFormat::Dxt1;
			}
			if (!caps.keep_dxt5 && caps.dxt1) format = TextureDxtFormat::Dxt1;
		}
	}
	return format;
}

uint32_t texture_level_count(uint32_t width, uint32_t height,
		uint32_t creation_flags) {
	uint32_t levels = 0;
	for (uint32_t side = std::min(width, height); side > 2; side >>= 1) {
		++levels;
	}
	if ((creation_flags & 0x40000u) != 0) levels = 1;
	if ((creation_flags & 0x80000u) != 0) levels = std::min(levels, 3u);
	if (levels == 0) {
		// D3DXCreateTexture's 0: every level down to 1x1.
		for (uint32_t side = std::max(width, height); side > 0; side >>= 1) {
			++levels;
		}
	}
	return levels;
}

size_t dxt_block_bytes(TextureDxtFormat format) {
	switch (format) {
	case TextureDxtFormat::Dxt1: return 8;
	case TextureDxtFormat::Dxt5: return 16;
	case TextureDxtFormat::None: break;
	}
	return 0;
}

void encode_dxt1_block(const DxtBlockColors &texels, bool dither,
		uint8_t *block) {
	if (!dither) {
		encode_color_block(texels.data(), block, true, false);
		return;
	}
	// Dithering first diffuses alpha to 0 or 1.
	// [orig: D3DXTex_D3DXEncodeDXT1 @ 0x72171E..0x721805]
	float error[16] = {};
	DxtColor dithered[16];
	for (uint32_t i = 0; i < 16; ++i) {
		dithered[i].r = texels[i].r;
		dithered[i].g = texels[i].g;
		dithered[i].b = texels[i].b;
		const float alpha = error[i] + texels[i].a;
		const float quantized = static_cast<float>(static_cast<int32_t>(alpha + 0.5f));
		dithered[i].a = quantized;
		diffuse_error(error, 1, i, alpha - quantized);
	}
	encode_color_block(dithered, block, true, true);
}

void encode_dxt5_block(const DxtBlockColors &texels, bool dither,
		uint8_t *block) {
	// [orig: D3DXTex_EncodeDXT5Block @ 0x72196E..0x721A88]
	float min_alpha = texels[0].a;
	float max_alpha = texels[0].a;
	float error[16] = {};
	float alpha[16];
	for (uint32_t i = 0; i < 16; ++i) {
		float value = texels[i].a;
		if (dither) value = value + error[i];
		const float scaled = value * 255.0f;
		alpha[i] = static_cast<float>(static_cast<int32_t>(scaled + 0.5f)) * kInv255;
		if (alpha[i] < min_alpha) {
			min_alpha = alpha[i];
		} else if (alpha[i] > max_alpha) {
			max_alpha = alpha[i];
		}
		if (dither) diffuse_error(error, 1, i, value - alpha[i]);
	}

	// [orig: @ 0x721A9F]
	encode_color_block(texels.data(), block + 8, false, dither);

	// [orig: @ 0x721AAC..0x721AC5, zero indices @ 0x721B95]
	if (1.0f == min_alpha) {
		block[0] = 0xFF;
		block[1] = 0xFF;
		std::memset(block + 2, 0, 6);
		return;
	}
	const uint32_t steps = (0.0f == min_alpha || 1.0f == max_alpha) ? 6 : 8;
	float alpha_a;
	float alpha_b;
	optimize_alpha(alpha_a, alpha_b, alpha, steps);

	// [orig: @ 0x721B12..0x721B9D]
	float scaled = alpha_a * 255.0f;
	const uint8_t byte_a = static_cast<uint8_t>(static_cast<int32_t>(scaled + 0.5f));
	scaled = alpha_b * 255.0f;
	const uint8_t byte_b = static_cast<uint8_t>(static_cast<int32_t>(scaled + 0.5f));
	alpha_a = static_cast<float>(byte_a) * kInv255;
	alpha_b = static_cast<float>(byte_b) * kInv255;
	if (steps == 8 && byte_a == byte_b) {
		block[0] = byte_a;
		block[1] = byte_b;
		std::memset(block + 2, 0, 6);
		return;
	}

	// [orig: @ 0x721BA2..0x721C55]
	float step[8];
	const uint32_t *palette_index;
	if (steps == 6) {
		block[0] = byte_a;
		block[1] = byte_b;
		step[0] = alpha_a;
		step[1] = alpha_b;
		for (uint32_t i = 1; i < 5; ++i) {
			step[i + 1] = (static_cast<float>(5 - i) * step[0] +
					static_cast<float>(i) * step[1]) * kFifth;
		}
		step[6] = 0.0f;
		step[7] = 1.0f;
		palette_index = kSteps6;
	} else {
		block[0] = byte_b;
		block[1] = byte_a;
		step[0] = alpha_b;
		step[1] = alpha_a;
		for (uint32_t i = 1; i < 7; ++i) {
			step[i + 1] = (static_cast<float>(7 - i) * step[0] +
					static_cast<float>(i) * step[1]) * kSeventh;
		}
		palette_index = kSteps8;
	}

	// [orig: @ 0x721C5C..0x721C90]
	const float f_steps = static_cast<float>(steps - 1);
	const float scale = step[0] == step[1] ? 0.0f : f_steps / (step[1] - step[0]);

	// Indices come from the unquantized alpha, eight texels per 24 bits.
	// [orig: @ 0x721C93..0x721E24]
	std::fill(std::begin(error), std::end(error), 0.0f);
	for (uint32_t set = 0; set < 2; ++set) {
		uint32_t indices = 0;
		for (uint32_t i = set * 8; i < set * 8 + 8; ++i) {
			float value = texels[i].a;
			if (dither) value = value + error[i];
			const float dot = (value - step[0]) * scale;
			uint32_t index;
			if (!(dot > 0.0f)) {
				index = (steps == 6 && step[0] * 0.5f >= value) ? 6 : 0;
			} else if (!(dot < f_steps)) {
				index = (steps == 6 && (step[1] + 1.0f) * 0.5f <= value) ? 7 : 1;
			} else {
				index = palette_index[static_cast<int32_t>(dot + 0.5f)];
			}
			indices = (index << 21) | (indices >> 3);
			if (dither) diffuse_error(error, 1, i, value - step[index]);
		}
		block[2 + set * 3] = static_cast<uint8_t>(indices);
		block[3 + set * 3] = static_cast<uint8_t>(indices >> 8);
		block[4 + set * 3] = static_cast<uint8_t>(indices >> 16);
	}
}

void decode_dxt1_block(const uint8_t *block, DxtBlockColors &texels) {
	const uint16_t word0 = read_u16(block);
	const uint16_t word1 = read_u16(block + 2);
	DxtColor palette[4];
	palette[0] = decode_565(word0);
	palette[1] = decode_565(word1);
	const DxtColor &c0 = palette[0];
	const DxtColor &c1 = palette[1];
	if (word0 <= word1) {
		// [orig: @ 0x72143C..0x72149A]
		palette[2] = {(c1.r - c0.r) * 0.5f + c0.r, (c1.g - c0.g) * 0.5f + c0.g,
				(c1.b - c0.b) * 0.5f + c0.b, (c1.a - c0.a) * 0.5f + c0.a};
		palette[3] = DxtColor{};
	} else {
		// [orig: @ 0x72149F..0x72151D]
		const float d_r = c1.r - c0.r;
		const float d_g = c1.g - c0.g;
		const float d_b = c1.b - c0.b;
		const float d_a = c1.a - c0.a;
		palette[2] = {d_r * kThird + c0.r, d_g * kThird + c0.g,
				d_b * kThird + c0.b, d_a * kThird + c0.a};
		palette[3] = {d_r * kTwoThirds + c0.r, d_g * kTwoThirds + c0.g,
				d_b * kTwoThirds + c0.b, d_a * kTwoThirds + c0.a};
	}
	// [orig: @ 0x72151F..0x721541]
	uint32_t indices = static_cast<uint32_t>(block[4]) |
			(static_cast<uint32_t>(block[5]) << 8) |
			(static_cast<uint32_t>(block[6]) << 16) |
			(static_cast<uint32_t>(block[7]) << 24);
	for (uint32_t i = 0; i < 16; ++i) {
		texels[i] = palette[indices & 3];
		indices >>= 2;
	}
}

void decode_dxt5_block(const uint8_t *block, DxtBlockColors &texels) {
	decode_dxt1_block(block + 8, texels);
	// [orig: @ 0x7215F3..0x7216A9]
	float alpha[8];
	alpha[0] = static_cast<float>(block[0]) * kInv255;
	alpha[1] = static_cast<float>(block[1]) * kInv255;
	if (block[0] > block[1]) {
		for (uint32_t i = 1; i < 7; ++i) {
			alpha[i + 1] = (static_cast<float>(7 - i) * alpha[0] +
					static_cast<float>(i) * alpha[1]) * kSeventh;
		}
	} else {
		for (uint32_t i = 1; i < 5; ++i) {
			alpha[i + 1] = (static_cast<float>(5 - i) * alpha[0] +
					static_cast<float>(i) * alpha[1]) * kFifth;
		}
		alpha[6] = 0.0f;
		alpha[7] = 1.0f;
	}
	// [orig: @ 0x7216AC..0x721705]
	for (uint32_t set = 0; set < 2; ++set) {
		uint32_t indices = static_cast<uint32_t>(block[2 + set * 3]) |
				(static_cast<uint32_t>(block[3 + set * 3]) << 8) |
				(static_cast<uint32_t>(block[4 + set * 3]) << 16);
		for (uint32_t i = 0; i < 8; ++i) {
			texels[set * 8 + i].a = alpha[indices & 7];
			indices >>= 3;
		}
	}
}

bool DxtSurface::is_valid() const noexcept {
	const size_t bytes = dxt_block_bytes(format);
	return bytes != 0 && width != 0 && height != 0 &&
			blocks.size() == static_cast<size_t>((width + 3) / 4) *
					((height + 3) / 4) * bytes;
}

std::vector<DxtColor> decode_rgba8(const uint8_t *rgba, uint32_t width,
		uint32_t height) {
	std::vector<DxtColor> colors(static_cast<size_t>(width) * height);
	for (size_t i = 0; i < colors.size(); ++i) {
		const uint8_t *pixel = rgba + i * 4;
		colors[i] = {static_cast<float>(pixel[0]) * kInv255,
				static_cast<float>(pixel[1]) * kInv255,
				static_cast<float>(pixel[2]) * kInv255,
				static_cast<float>(pixel[3]) * kInv255};
	}
	return colors;
}

std::vector<uint8_t> encode_rgba8(const std::vector<DxtColor> &colors) {
	const auto to_byte = [](float value) {
		const float scaled = value * 255.0f;
		const int32_t rounded = static_cast<int32_t>(scaled + 0.5f);
		return static_cast<uint8_t>(rounded >= 0xFF ? 0xFF : (rounded <= 0 ? 0 : rounded));
	};
	std::vector<uint8_t> rgba(colors.size() * 4);
	for (size_t i = 0; i < colors.size(); ++i) {
		rgba[i * 4 + 0] = to_byte(colors[i].r);
		rgba[i * 4 + 1] = to_byte(colors[i].g);
		rgba[i * 4 + 2] = to_byte(colors[i].b);
		rgba[i * 4 + 3] = to_byte(colors[i].a);
	}
	return rgba;
}

DxtSurface encode_dxt_surface(const std::vector<DxtColor> &colors,
		uint32_t width, uint32_t height, TextureDxtFormat format) {
	DxtSurface surface;
	const size_t block_bytes = dxt_block_bytes(format);
	if (block_bytes == 0 || width == 0 || height == 0 ||
			colors.size() != static_cast<size_t>(width) * height) {
		return surface;
	}
	surface.format = format;
	surface.width = width;
	surface.height = height;
	const uint32_t blocks_x = (width + 3) / 4;
	const uint32_t blocks_y = (height + 3) / 4;
	surface.blocks.resize(static_cast<size_t>(blocks_x) * blocks_y * block_bytes);
	// Past an edge, texel column (row) 1 and 2 repeat texel 0 and column 3
	// repeats column 1. [orig: v36 = {0, 0, 0, 1} @ 0x6EDE25..0x6EDE34]
	static constexpr uint32_t kEdgeSource[4] = {0, 0, 0, 1};
	for (uint32_t by = 0; by < blocks_y; ++by) {
		for (uint32_t bx = 0; bx < blocks_x; ++bx) {
			DxtBlockColors texels{};
			const uint32_t valid_w = std::min(4u, width - bx * 4);
			const uint32_t valid_h = std::min(4u, height - by * 4);
			for (uint32_t y = 0; y < valid_h; ++y) {
				for (uint32_t x = 0; x < valid_w; ++x) {
					texels[y * 4 + x] = colors[static_cast<size_t>(by * 4 + y) * width +
							bx * 4 + x];
				}
			}
			if (valid_w < 4) {
				for (uint32_t y = 0; y < valid_h; ++y) {
					for (uint32_t x = valid_w; x < 4; ++x) {
						texels[y * 4 + x] = texels[y * 4 + kEdgeSource[x]];
					}
				}
			}
			for (uint32_t y = valid_h; y < 4; ++y) {
				for (uint32_t x = 0; x < 4; ++x) {
					texels[y * 4 + x] = texels[kEdgeSource[y] * 4 + x];
				}
			}
			uint8_t *block = surface.blocks.data() +
					(static_cast<size_t>(by) * blocks_x + bx) * block_bytes;
			// The blit never asks for dithering here: the texture creator
			// loads with D3DX_FILTER_NONE and filters with D3DX_FILTER_BOX.
			// [orig: D3DXTex::CBlt::Blt @ 0x6E30F2..0x6E311A (dither = filter
			// & 0x80000); D3DXTex::CCodecDXT::Encode @ 0x6EDECE..0x6EDEDF]
			if (format == TextureDxtFormat::Dxt1) {
				encode_dxt1_block(texels, false, block);
			} else {
				encode_dxt5_block(texels, false, block);
			}
		}
	}
	return surface;
}

std::vector<DxtColor> decode_dxt_surface(const DxtSurface &surface) {
	std::vector<DxtColor> colors;
	if (!surface.is_valid()) return colors;
	colors.resize(static_cast<size_t>(surface.width) * surface.height);
	const size_t block_bytes = dxt_block_bytes(surface.format);
	const uint32_t blocks_x = (surface.width + 3) / 4;
	const uint32_t blocks_y = (surface.height + 3) / 4;
	for (uint32_t by = 0; by < blocks_y; ++by) {
		for (uint32_t bx = 0; bx < blocks_x; ++bx) {
			const uint8_t *block = surface.blocks.data() +
					(static_cast<size_t>(by) * blocks_x + bx) * block_bytes;
			DxtBlockColors texels;
			if (surface.format == TextureDxtFormat::Dxt1) {
				decode_dxt1_block(block, texels);
			} else {
				decode_dxt5_block(block, texels);
			}
			for (uint32_t y = 0; y < 4 && by * 4 + y < surface.height; ++y) {
				for (uint32_t x = 0; x < 4 && bx * 4 + x < surface.width; ++x) {
					colors[static_cast<size_t>(by * 4 + y) * surface.width + bx * 4 + x] =
							texels[y * 4 + x];
				}
			}
		}
	}
	return colors;
}

std::vector<DxtColor> box_filter_half(const std::vector<DxtColor> &colors,
		uint32_t width, uint32_t height) {
	if (width == 0 || height == 0 || colors.size() != static_cast<size_t>(width) * height) {
		return {};
	}
	const uint32_t out_w = std::max(1u, width / 2);
	const uint32_t out_h = std::max(1u, height / 2);
	std::vector<DxtColor> result(static_cast<size_t>(out_w) * out_h);
	for (uint32_t y = 0; y < out_h; ++y) {
		const uint32_t row0 = height == 1 ? 0 : y * 2;
		const uint32_t row1 = height == 1 ? 0 : y * 2 + 1;
		for (uint32_t x = 0; x < out_w; ++x) {
			const uint32_t col0 = width == 1 ? 0 : x * 2;
			const uint32_t col1 = width == 1 ? 0 : x * 2 + 1;
			const DxtColor &p00 = colors[static_cast<size_t>(row0) * width + col0];
			const DxtColor &p01 = colors[static_cast<size_t>(row0) * width + col1];
			const DxtColor &p10 = colors[static_cast<size_t>(row1) * width + col0];
			const DxtColor &p11 = colors[static_cast<size_t>(row1) * width + col1];
			DxtColor &out = result[static_cast<size_t>(y) * out_w + x];
			out.r = (((p01.r + p00.r) + p10.r) + p11.r) * 0.25f;
			out.g = (((p01.g + p00.g) + p10.g) + p11.g) * 0.25f;
			out.b = (((p01.b + p00.b) + p10.b) + p11.b) * 0.25f;
			out.a = (((p01.a + p00.a) + p10.a) + p11.a) * 0.25f;
		}
	}
	return result;
}

std::vector<uint8_t> box_filter_half_rgba8(const uint8_t *rgba, uint32_t width, uint32_t height) {
	return encode_rgba8(box_filter_half(decode_rgba8(rgba, width, height), width, height));
}

bool decode_dds_levels(const uint8_t *bytes, size_t size, dds::DdsImage &image, size_t max_levels) {
	const dds::DdsFormat &format = image.format;
	const bool dxt1 = format.d3d == dds::dds_fourcc('D', 'X', 'T', '1');
	const bool dxt5 = format.d3d == dds::dds_fourcc('D', 'X', 'T', '4') ||
			format.d3d == dds::dds_fourcc('D', 'X', 'T', '5');
	if (!dxt1 && !dxt5)
		return format.decoded;
	size_t decoded = 0;
	for (dds::DdsLevel &level : image.levels) {
		if (max_levels != 0 && decoded >= max_levels)
			break;
		++decoded;
		if (bytes == nullptr || level.offset > size || level.bytes > size - level.offset)
			continue;
		DxtSurface surface;
		surface.format = dxt1 ? TextureDxtFormat::Dxt1 : TextureDxtFormat::Dxt5;
		surface.width = level.width;
		surface.height = level.height;
		surface.blocks.assign(bytes + level.offset, bytes + level.offset + level.bytes);
		level.rgba = encode_rgba8(decode_dxt_surface(surface));
	}
	return true;
}

std::vector<DxtSurface> build_dxt_texture_levels(const uint8_t *rgba,
		uint32_t width, uint32_t height, TextureDxtFormat format,
		uint32_t level_count) {
	std::vector<DxtSurface> levels;
	if (rgba == nullptr || width == 0 || height == 0 || level_count == 0 ||
			dxt_block_bytes(format) == 0) {
		return levels;
	}
	levels.push_back(encode_dxt_surface(decode_rgba8(rgba, width, height),
			width, height, format));
	uint32_t level_w = width;
	uint32_t level_h = height;
	while (levels.size() < level_count && (level_w > 1 || level_h > 1)) {
		const std::vector<DxtColor> filtered =
				box_filter_half(decode_dxt_surface(levels.back()), level_w, level_h);
		level_w = std::max(1u, level_w / 2);
		level_h = std::max(1u, level_h / 2);
		levels.push_back(encode_dxt_surface(filtered, level_w, level_h, format));
	}
	return levels;
}

} // namespace opennova::renderer
