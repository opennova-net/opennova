#include "renderer/uv_anim.h"

#include "threedi/threedi_panm.h"

#include <cmath>

namespace renderer {

namespace {

constexpr double kInv65536 = 0.000015258789;     // [orig: flt operand @ 0x5b19f5]
constexpr double kInv65535 = 0.000015259022;     // waveform scale [orig: @ 0x5b1b52]
constexpr double kTwoPiOver65536 = 0.000095873802; // rotation angle unit [orig: @ 0x5b1a53]
constexpr double kTwoPi = 6.2831855;             // controlled rotation [orig: @ 0x5b1c86]
constexpr double kFrom8_8 = 0.00390625;          // base/range 8.8 [orig: @ 0x5b1ae6]

// Channel phase accumulator: (phase << 8) + time * speed, wrapping uint16
// [orig: @ 0x5b19c8..0x5b19e2].
uint16_t channel_phase16(const UvAnimChannel &ch, uint16_t time_units16) {
	const uint32_t sum =
			(static_cast<uint32_t>(ch.phase) << 8) +
			static_cast<uint32_t>(time_units16) *
					static_cast<uint32_t>(static_cast<uint16_t>(ch.speed));
	return static_cast<uint16_t>(sum);
}

// Waveform/controlled value in [base, base+range] as float
// [orig: @ 0x5b1ae6..0x5b1b6a waveform; @ 0x5b1c30 controlled].
double window_value(const UvAnimChannel &ch, double normalized_fraction) {
	const double base = static_cast<double>(ch.base) * kFrom8_8;
	const double range = static_cast<double>(ch.range) * kFrom8_8 - base;
	return base + range * normalized_fraction;
}

} // namespace

int32_t uv_anim_wave_lookup(uint8_t type, uint16_t phase16, uint16_t rand16) {
	const uint8_t *table = threedi_panm_wave_table();
	const uint8_t hi = static_cast<uint8_t>(phase16 >> 8);
	const uint8_t lo = static_cast<uint8_t>(phase16 & 0xFF);
	switch (type & 0xF) {
		case 1: return static_cast<int32_t>(table[hi]) << 8;
		case 2: return static_cast<int32_t>(table[hi + 256]) << 8;
		case 3: return static_cast<int32_t>(table[hi + 768]) << 8;
		case 4: return static_cast<int32_t>(table[hi + 1024]) << 8;
		case 5: return static_cast<int32_t>(table[hi + 1280]) << 8;
		case 6: return 16 * (rand16 & 0xFFF);
		case 7: {
			const int32_t a = table[1536 + hi];
			const int32_t b = table[1536 + static_cast<uint8_t>(hi + 1)];
			return (a << 8) + lo * (b - a);
		}
		case 8: return static_cast<int32_t>(table[hi + 1792]) << 8;
		case 9: return static_cast<int32_t>(table[hi + 2048]) << 8;
		case 0xA: {
			const int32_t a = table[2304 + hi];
			const int32_t b = table[2304 + static_cast<uint8_t>(hi + 1)];
			return (a << 8) + lo * (b - a);
		}
		case 0xF: return static_cast<int32_t>(table[hi + 2560]) << 8;
		default: return 0;
	}
}

UvAnimTransform uv_anim_transform(const UvAnimChannel &u_channel,
                                  const UvAnimChannel &v_channel,
                                  uint16_t time_units16,
                                  int32_t controlled_u,
                                  int32_t controlled_v,
                                  uint16_t rand16_u,
                                  uint16_t rand16_v) {
	// memset-zero start; m00/m11 become 1 only where a path sets them
	// [orig: @ 0x5b19a4 memset 0x40; identity diag fill 0x28E09B8/0x28E09CC].
	UvAnimTransform t;
	t.m00 = 0.0f;
	t.m11 = 0.0f;

	// --- U channel: writes m00 (u-scale), m10 (u-from-v), m20 (u-translate) ---
	{
		const UvAnimChannel &ch = u_channel;
		const uint8_t mode = ch.type & 0xF0;
		if (mode == 0) {
			t.m00 = 1.0f; // identity U [orig: @ 0x5b1d20]
		} else {
			const uint16_t phase16 = channel_phase16(ch, time_units16);
			if (mode == 0x10) {
				// time scroll: translate = phase/65536, type 16 = +, 17 = -
				// [orig: @ 0x5b19f5..0x5b1a34]
				t.m00 = 1.0f;
				const double v = static_cast<double>(phase16) * kInv65536;
				if (ch.type == 16)
					t.m20 = static_cast<float>(v);
				else if (ch.type == 17)
					t.m20 = static_cast<float>(-v);
			} else if (mode == 0x20) {
				// rotation about UV center, angle = phase * 2pi/65536,
				// type 32 = +, 33 = - [orig: @ 0x5b1a45..0x5b1ad2]
				const double a = static_cast<double>(phase16) * kTwoPiOver65536;
				const double c = std::cos(a);
				const double s = std::sin(a);
				t.m00 = static_cast<float>(c);
				if (ch.type == 32) {
					t.m10 = static_cast<float>(s);
					t.m20 = static_cast<float>(0.5 - (c + s) * 0.5);
				} else if (ch.type == 33) {
					t.m10 = static_cast<float>(-s);
					t.m20 = static_cast<float>(0.5 - (c - s) * 0.5);
				}
			} else if (ch.type <= 0x70) {
				// waveform value in [base, end], wave/65535
				// [orig: @ 0x5b1ae6..0x5b1c1c]
				const double w = static_cast<double>(
						uv_anim_wave_lookup(ch.type, phase16, rand16_u)) *
						kInv65535;
				const double base = static_cast<double>(ch.base) * kFrom8_8;
				const double range = static_cast<double>(ch.range) * kFrom8_8 - base;
				const float v = static_cast<float>(w * range + base);
				switch (mode) {
					case 0x30: t.m20 = v; break;                    // U set (m00 stays 0)
					case 0x40: t.m00 = 1.0f; t.m20 = v; break;      // scroll
					case 0x50: t.m00 = 1.0f; t.m10 = v; break;      // shear
					case 0x60: t.m00 = v; break;                    // scale
					default: break;
				}
			} else {
				// controlled-animation value, base + range*ctrl/65536
				// [orig: @ 0x5b1c30..0x5b1cb2]
				const double frac = static_cast<double>(controlled_u) * kInv65536;
				const float cv = static_cast<float>(window_value(ch, frac));
				switch (ch.type) {
					case 'q': t.m20 = cv; break;
					case 'r': t.m00 = 1.0f; t.m20 = cv; break;
					case 's': t.m00 = 1.0f; t.m10 = cv; break;
					case 't': t.m00 = cv; break;
					case 'u': {
						const double a = static_cast<double>(cv) * kTwoPi;
						const double c = std::cos(a);
						const double s = std::sin(a);
						t.m00 = static_cast<float>(c);
						t.m10 = static_cast<float>(s);
						t.m20 = static_cast<float>(0.5 - (c + s) * 0.5);
						break;
					}
					default: break;
				}
			}
		}
	}

	// --- V channel: writes m01 (v-from-u), m11 (v-scale), m21 (v-translate) ---
	{
		const UvAnimChannel &ch = v_channel;
		const uint8_t mode = ch.type & 0xF0;
		if (mode == 0) {
			t.m11 = 1.0f; // identity V [orig: LABEL_54 @ 0x5b1ef0]
		} else {
			const uint16_t phase16 = channel_phase16(ch, time_units16);
			if (mode == 0x10) {
				// [orig: @ 0x5b1d4a..0x5b1d9d]
				t.m11 = 1.0f;
				const double v = static_cast<double>(phase16) * kInv65536;
				if (ch.type == 16)
					t.m21 = static_cast<float>(v);
				else if (ch.type == 17)
					t.m21 = static_cast<float>(-v);
			} else if (mode == 0x20) {
				// [orig: @ 0x5b1f0a..0x5b1f7f]
				const double a = static_cast<double>(phase16) * kTwoPiOver65536;
				const double c = std::cos(a);
				const double s = std::sin(a);
				t.m11 = static_cast<float>(c);
				if (ch.type == 32) {
					t.m01 = static_cast<float>(-s);
					t.m21 = static_cast<float>(0.5 - (c - s) * 0.5);
				} else if (ch.type == 33) {
					t.m01 = static_cast<float>(s);
					t.m21 = static_cast<float>(0.5 - (s + c) * 0.5);
				}
			} else if (ch.type <= 0x70) {
				// [orig: @ 0x5b1da6..0x5b1e4a]
				const double w = static_cast<double>(
						uv_anim_wave_lookup(ch.type, phase16, rand16_v)) *
						kInv65535;
				const double base = static_cast<double>(ch.base) * kFrom8_8;
				const double range = static_cast<double>(ch.range) * kFrom8_8 - base;
				const float v = static_cast<float>(w * range + base);
				switch (mode) {
					case 0x30: t.m21 = v; break;                    // V set (m11 stays 0)
					case 0x40: t.m11 = 1.0f; t.m21 = v; break;      // scroll
					case 0x50: t.m01 = v; t.m11 = 1.0f; break;      // shear
					case 0x60: t.m11 = v; break;                    // scale
					default: break;
				}
			} else {
				// [orig: @ 0x5b1e53..0x5b1ee9]
				const double frac = static_cast<double>(controlled_v) * kInv65536;
				const float cv = static_cast<float>(window_value(ch, frac));
				switch (ch.type) {
					case 'q': t.m21 = cv; break;
					case 'r': t.m11 = 1.0f; t.m21 = cv; break;
					case 's': t.m01 = cv; t.m11 = 1.0f; break;
					case 't': t.m11 = cv; break;
					case 'u': {
						const double a = static_cast<double>(cv) * kTwoPi;
						const double c = std::cos(a);
						const double s = std::sin(a);
						t.m01 = static_cast<float>(-s);
						t.m11 = static_cast<float>(c);
						t.m21 = static_cast<float>(0.5 - (c - s) * 0.5);
						break;
					}
					default: break;
				}
			}
		}
	}

	return t;
}

} // namespace renderer
