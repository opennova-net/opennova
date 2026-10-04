// D3DX's PPM, PFM, Radiance HDR and DIB codecs, structural ports of the codecs
// `D3DXTex::CImage::Load @ 0x6DF1DC` tries on a "DDS" file's bytes (its codec table
// @ 0x6DF212..0x6DF242, the dispatch @ 0x6DF2A6). Load sets LC_NUMERIC to "C" around
// the codecs (@ 0x6DF249..0x6DF278), so a number's decimal point is '.'.
//
// Character classes: the codecs call the CRT's isspace / isdigit and sscanf, whose
// LC_CTYPE the game sets to the system ANSI code page
// (`System_InitTimerAndLocale @ 0x762A6E`, setlocale(LC_ALL, ".ACP")). The port
// classifies ASCII only; a byte >= 0x80 the user's code page calls a space or a digit
// (cp1252's 0xA0, say) is not one here.
#include <runtime/renderer/d3dx_image_codecs.h>

#include <base/io/le.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace opennova::renderer {

namespace {

bool fail(std::string &error, const char *why) {
	error = why;
	return false;
}

bool is_c_space(uint8_t c) {
	return c == ' ' || (c >= '\t' && c <= '\r');
}

bool is_c_digit(uint8_t c) {
	return c >= '0' && c <= '9';
}

// Whether a width x height image is within the port's bound (and has pixels).
bool pixel_count_ok(uint64_t width, uint64_t height) {
	return width != 0 && height != 0 && width * height <= kMaxD3dxImagePixels;
}

// [orig: D3DXTex::findendl @ 0x6DB39F] The index of the first '\n' in [0, limit);
// 0 when there is none, and also when the '\n' is the first byte.
size_t findendl(const uint8_t *p, size_t limit) {
	if (limit == 0) return 0;
	size_t i = 0;
	while (p[i] != '\n') {
		if (++i >= limit) return 0;
	}
	return i;
}

// [orig: StringCchCopyNA @ 0x6DE1AF -> StringCopyWorkerA @ 0x6DE0C3] A line copied
// into the codec's 256-byte buffer: its `length` bytes, cut at the first NUL.
std::string copy_line(const uint8_t *p, size_t length) {
	size_t n = 0;
	while (n < length && p[n] != 0) ++n;
	return std::string(reinterpret_cast<const char *>(p), n);
}

// The CRT sscanf conversions the codecs use. %u: leading whitespace, an optional
// sign, at least one digit; the value wraps modulo 2^32 and a '-' negates it.
bool scan_u32(const char *&p, uint32_t &out) {
	while (is_c_space(static_cast<uint8_t>(*p))) ++p;
	bool negative = false;
	if (*p == '-') {
		negative = true;
		++p;
	} else if (*p == '+') {
		++p;
	}
	if (!is_c_digit(static_cast<uint8_t>(*p))) return false;
	uint32_t value = 0;
	while (is_c_digit(static_cast<uint8_t>(*p))) {
		value = value * 10u + static_cast<uint32_t>(*p - '0');
		++p;
	}
	out = negative ? 0u - value : value;
	return true;
}

// %f into a float: leading whitespace, an optional sign, digits, an optional '.' and
// digits (at least one digit in all), then, after a digit, an optional exponent letter
// with an optional sign and digits; the letter is taken even with no digits after it
// ("1e" reads 1) [orig: sscanf @ 0x76C5D8 -> CRT_input_l @ 0x77B155, the exponent
// @ 0x77B604..0x77B6AC]. No "inf" or "nan". The value is the correctly rounded float
// (the CRT's own conversion is not re-witnessed), converted from a decimal-point-free
// spelling so the process's own locale cannot change it.
bool scan_float(const char *&p, float &out) {
	while (is_c_space(static_cast<uint8_t>(*p))) ++p;
	bool negative = false;
	if (*p == '-') {
		negative = true;
		++p;
	} else if (*p == '+') {
		++p;
	}
	std::string digits;
	long long exponent = 0;
	bool started = false;
	while (is_c_digit(static_cast<uint8_t>(*p))) {
		started = true;
		digits += *p++;
	}
	if (*p == '.') {
		++p;
		while (is_c_digit(static_cast<uint8_t>(*p))) {
			started = true;
			digits += *p++;
			--exponent;
		}
	}
	if (started && (*p == 'e' || *p == 'E')) {
		++p;
		bool exponent_negative = false;
		if (*p == '-') {
			exponent_negative = true;
			++p;
		} else if (*p == '+') {
			++p;
		}
		long long written = 0;
		while (is_c_digit(static_cast<uint8_t>(*p))) {
			if (written < 1000000) written = written * 10 + (*p - '0');
			++p;
		}
		exponent += exponent_negative ? -written : written;
	}
	if (!started) return false;
	size_t first = 0;
	while (first < digits.size() && digits[first] == '0') ++first;
	float magnitude = 0.0f;
	if (first < digits.size()) {
		const std::string spelled = digits.substr(first) + "e" + std::to_string(exponent);
		magnitude = std::strtof(spelled.c_str(), nullptr);
	}
	out = negative ? -magnitude : magnitude;
	return true;
}

// The trailing %s of "...%s": it converts (and the count grows past the one the codec
// wants) unless only whitespace remains.
bool only_blank(const char *p) {
	while (is_c_space(static_cast<uint8_t>(*p))) ++p;
	return *p == 0;
}

// sscanf(line, "%u %u%s", ...) == 2.
bool scan_u_u_only(const std::string &line, uint32_t &a, uint32_t &b) {
	const char *p = line.c_str();
	if (!scan_u32(p, a)) return false;
	return scan_u32(p, b) && only_blank(p);
}

// sscanf(line, "%f%s", ...) == 1.
bool scan_f_only(const std::string &line, float &value) {
	const char *p = line.c_str();
	return scan_float(p, value) && only_blank(p);
}

// A double rounded to a float as an x87 store rounds it: past the largest float's
// rounding range it is an infinity.
float round_to_float(double value) {
	const double overflow = std::ldexp(1.0, 128) - std::ldexp(1.0, 103);
	if (value >= overflow) return std::numeric_limits<float>::infinity();
	if (value <= -overflow) return -std::numeric_limits<float>::infinity();
	return static_cast<float>(value);
}

void put_rgba(std::vector<uint8_t> &pixels, size_t index, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
	uint8_t *dst = pixels.data() + index * 4u;
	dst[0] = r;
	dst[1] = g;
	dst[2] = b;
	dst[3] = a;
}

void put_float_rgb(std::vector<uint8_t> &pixels, size_t index, float r, float g, float b) {
	// The codecs' alpha is 1.0 [orig: PFM @ 0x6DE3F5..0x6DE3F7; HDR @ 0x6DF0AF..0x6DF0B1].
	put_rgba(pixels, index, d3dx_float_to_unorm8(r), d3dx_float_to_unorm8(g), d3dx_float_to_unorm8(b),
			d3dx_float_to_unorm8(1.0f));
}

} // namespace

// [orig: D3DXTex::CCodec_A8R8G8B8::Encode @ 0x6E57F9 — the channel (plus the zero
//  diffusion error) times 255.0 stored as a float @ 0x6E5906..0x6E590E, plus the bias
//  stored as a float @ 0x6E5931..0x6E5936 (0.5 from the table @ 0x856B90 that
//  CCodec::CCodec @ 0x6E4D38..0x6E4D47 picks when the blit does not dither; the game's
//  Filter D3DX_FILTER_NONE has no dither bit), fistp under the chop rounding the encoder
//  sets @ 0x6E588F..0x6E589D, clamped to 0..255 @ 0x6E5AC0..0x6E5B5E. An out-of-range
//  fistp stores 0x80000000, which the clamp turns to 0.]
uint8_t d3dx_float_to_unorm8(float value) {
	const double product = static_cast<double>(value) * 255.0;
	if (std::isnan(product) || product >= 2147483648.0 || product <= 0.0) return 0;
	const float scaled = static_cast<float>(product);
	const float biased = static_cast<float>(static_cast<double>(scaled) + 0.5);
	if (!(biased < 2147483648.0f)) return 0;
	const int32_t truncated = static_cast<int32_t>(biased);
	if (truncated <= 0) return 0;
	return truncated >= 255 ? 255 : static_cast<uint8_t>(truncated);
}

// [orig: D3DXTex::CImage::LoadPPM @ 0x6DD011]
bool decode_d3dx_ppm(const uint8_t *data, size_t size, opennova::RgbaImage &out, std::string &error) {
	out = opennova::RgbaImage{};
	// Under two bytes, or anything but "P3" / "P6", fails [orig: @ 0x6DD019..0x6DD056].
	if (data == nullptr || size < 2) return fail(error, "PPM: under two bytes");
	if (data[0] != 'P') return fail(error, "PPM: no 'P'");
	bool ascii = false;
	if (data[1] == '3')
		ascii = true;
	else if (data[1] != '6')
		return fail(error, "PPM: neither P3 nor P6");
	size_t at = 2;
	size_t remaining = size - 2;
	if (remaining == 0) return fail(error, "PPM: nothing after the magic"); // [orig: @ 0x6DD07A]
	uint32_t width = 0;
	uint32_t maxval = 255;
	int state = 0; // 0 width, 1 height, 2 maxval, 3..5 an ASCII pixel's red, green, blue
	size_t pixels = 0;
	size_t next = 0;
	uint32_t word = 0;
	// Each sample scaled by 255 / maxval, 32-bit [orig: imul 0FFh, div @ 0x6DD11E..0x6DD126].
	const auto level = [&](uint32_t sample) { return static_cast<uint32_t>(sample * 255u) / maxval; };
	// The X8R8G8B8 word, read as R, G, B and an opaque alpha (the texture's X channel).
	const auto store = [&](size_t index, uint32_t w) {
		put_rgba(out.pixels, index, static_cast<uint8_t>(w >> 16), static_cast<uint8_t>(w >> 8),
				static_cast<uint8_t>(w), 255);
	};
	// The header (and, for P3, every sample): whitespace skipped, a '#' comment skipped
	// through its '\n', anything else a run of digits ended by whitespace; the data
	// running out anywhere fails [orig: the loop @ 0x6DD080..0x6DD1EE].
	while (ascii || state != 3) {
		const uint8_t c = data[at];
		if (is_c_space(c)) {
			++at;
			--remaining;
		} else if (c == '#') {
			// [orig: @ 0x6DD0A9..0x6DD0BE] Retail steps past its buffer when the comment
			// has no '\n' and then fails on the next token; the port fails here.
			while (remaining != 0 && data[at] != '\n') {
				++at;
				--remaining;
			}
			if (remaining == 0) return fail(error, "PPM: a comment runs past the data");
			++at;
			--remaining;
		} else {
			// [orig: @ 0x6DD0C0..0x6DD0F7] A non-digit before the token's whitespace fails;
			// the value wraps modulo 2^32.
			uint32_t value = 0;
			while (remaining != 0) {
				const uint8_t d = data[at];
				if (is_c_space(d)) break;
				if (!is_c_digit(d)) return fail(error, "PPM: a token holds a non-digit");
				value = d + value * 10u - '0';
				++at;
				--remaining;
			}
			switch (state) {
				case 0: // [orig: @ 0x6DD1DF..0x6DD1E4]
					width = value;
					if (value == 0) return fail(error, "PPM: zero width");
					break;
				case 1: { // [orig: @ 0x6DD18F..0x6DD1DD — the X8R8G8B8 image (format 22 @ 0x6DD1CB)]
					if (value == 0) return fail(error, "PPM: zero height");
					if (!pixel_count_ok(width, value)) return fail(error, "PPM: too many pixels");
					pixels = static_cast<size_t>(width) * value;
					// The port's bound: every sample needs a byte (ASCII: a digit and a
					// separator), so a header naming more than the rest can hold fails
					// before the buffer, as its parse would.
					const uint64_t least = ascii ? 6u * uint64_t(pixels) + 2u : 3u * uint64_t(pixels) + 3u;
					if (least > remaining) return fail(error, "PPM: the data cannot hold the image");
					out.width = static_cast<int>(width);
					out.height = static_cast<int>(value);
					out.pixels.assign(pixels * 4u, 0);
					break;
				}
				case 2: // [orig: @ 0x6DD18A, @ 0x6DD1E2..0x6DD1E4]
					maxval = value;
					if (value == 0) return fail(error, "PPM: zero maxval");
					break;
				case 3: // [orig: @ 0x6DD165..0x6DD186]
					if (next >= pixels) return fail(error, "PPM: past the image");
					word = (level(value) | 0xFFFFFF00u) << 16;
					break;
				case 4: // [orig: @ 0x6DD149..0x6DD15E]
					word |= level(value) << 8;
					break;
				case 5: // [orig: @ 0x6DD11C..0x6DD13D — the last pixel ends the decode]
					word |= level(value);
					store(next++, word);
					if (next == pixels) return true;
					state = 2;
					break;
				default:
					break;
			}
			++state;
		}
		if (remaining == 0) return fail(error, "PPM: the data ends early"); // [orig: @ 0x6DD1E9..0x6DD1EE]
	}
	// P6: a maxval past 255 fails [orig: @ 0x6DD200..0x6DD209]; one '\r' then one more
	// byte after it is skipped [orig: @ 0x6DD20B..0x6DD21B]; then three bytes a pixel
	// [orig: @ 0x6DD220..0x6DD272].
	if (maxval > 255) return fail(error, "PPM: P6 maxval past 255");
	if (remaining > 1 && data[at] == '\r') {
		++at;
		--remaining;
	}
	++at;
	--remaining;
	// Retail fails when the pixel bytes run out on a pixel boundary
	// [orig: @ 0x6DD274..0x6DD27E] and reads past its buffer otherwise; the port fails
	// both.
	if (remaining < 3u * uint64_t(pixels)) return fail(error, "PPM: the pixel data is short");
	for (size_t i = 0; i < pixels; ++i) {
		const uint32_t r = level(data[at]);
		const uint32_t g = level(data[at + 1]);
		const uint32_t b = level(data[at + 2]);
		at += 3;
		store(i, b | ((g | ((r | 0xFFFFFF00u) << 8)) << 8));
	}
	return true;
}

// [orig: D3DXTex_LoadPFMFromMemory @ 0x6DE1F5]
bool decode_d3dx_pfm(const uint8_t *data, size_t size, opennova::RgbaImage &out, std::string &error) {
	out = opennova::RgbaImage{};
	// Four bytes at least, then "PF\n" (RGB) or "Pf\n" (grey) [orig: @ 0x6DE201..0x6DE240].
	if (data == nullptr || size <= 3) return fail(error, "PFM: under four bytes");
	bool grey = false;
	if (std::memcmp(data, "Pf\n", 3) == 0)
		grey = true;
	else if (std::memcmp(data, "PF\n", 3) != 0)
		return fail(error, "PFM: neither PF nor Pf with a newline");
	size_t at = 3;
	size_t remaining = size - 3;
	// The size line within the next 256 bytes, sscanf "%u %u%s" == 2
	// [orig: @ 0x6DE247..0x6DE2A8].
	size_t length = findendl(data + at, remaining > 256 ? 256 : remaining);
	if (length == 0) return fail(error, "PFM: no size line");
	uint32_t width = 0, height = 0;
	if (!scan_u_u_only(copy_line(data + at, length), width, height)) return fail(error, "PFM: bad size line");
	at += length + 1;
	remaining -= length + 1;
	if (remaining == 0) return fail(error, "PFM: no scale line"); // [orig: @ 0x6DE2AE..0x6DE2BF]
	// The scale line, sscanf "%f%s" == 1 [orig: @ 0x6DE2C5..0x6DE318]; only its sign is
	// read: not below 0.0 (0.0 @ 0x7C3284; -0.0 included) means big-endian floats
	// [orig: @ 0x6DE31E..0x6DE334]. Its magnitude never scales the pixels.
	length = findendl(data + at, remaining > 256 ? 256 : remaining);
	if (length == 0) return fail(error, "PFM: no scale line");
	float scale = 0.0f;
	if (!scan_f_only(copy_line(data + at, length), scale)) return fail(error, "PFM: bad scale line");
	const bool big_endian = !(scale < 0.0f);
	at += length + 1;
	remaining -= length + 1;
	// Retail decodes a zero side to an empty image (whether the texture is then made is
	// not witnessed) and wraps a huge one; the port fails both.
	if (!pixel_count_ok(width, height)) return fail(error, "PFM: no pixels, or too many");
	const size_t channels = grey ? 1 : 3;
	const uint64_t need = 4u * uint64_t(width) * height * channels;
	if (remaining < need) return fail(error, "PFM: the pixel data is short"); // [orig: @ 0x6DE337..0x6DE358]
	out.width = static_cast<int>(width);
	out.height = static_cast<int>(height);
	out.pixels.assign(static_cast<size_t>(width) * height * 4u, 0);
	// [orig: D3DXTex::MSBConvert @ 0x6DB3C0 — the word byte-swapped] for big-endian.
	const auto sample = [&]() {
		uint32_t word = io::read_u32_le(data + at);
		at += 4;
		if (big_endian)
			word = (word >> 24) | ((word >> 8) & 0xFF00u) | ((word << 8) & 0xFF0000u) | (word << 24);
		float value;
		std::memcpy(&value, &word, sizeof(value));
		return value;
	};
	// The file's first row is the image's last [orig: @ 0x6DE3A1..0x6DE3A8 and
	// @ 0x6DE481..0x6DE484]; grey sets red, green and blue [orig: @ 0x6DE3D9..0x6DE418];
	// RGB reads three floats [orig: @ 0x6DE425..0x6DE47F].
	for (uint32_t file_row = 0; file_row < height; ++file_row) {
		const size_t row = static_cast<size_t>(height - 1u - file_row);
		for (uint32_t x = 0; x < width; ++x) {
			const size_t index = row * width + x;
			if (grey) {
				const float v = sample();
				put_float_rgb(out.pixels, index, v, v, v);
			} else {
				const float r = sample();
				const float g = sample();
				const float b = sample();
				put_float_rgb(out.pixels, index, r, g, b);
			}
		}
	}
	return true;
}

// [orig: D3DXTex_LoadHDRFromMemory @ 0x6DEA53]
bool decode_d3dx_hdr(const uint8_t *data, size_t size, opennova::RgbaImage &out, std::string &error) {
	out = opennova::RgbaImage{};
	// Eleven bytes at least, the first ten "#?RADIANCE" [orig: @ 0x6DEA5E..0x6DEA8B]; the
	// size is a signed int [orig: @ 0x6DEA96..0x6DEA9B].
	if (data == nullptr || size <= 10) return fail(error, "HDR: under eleven bytes");
	if (std::memcmp(data, "#?RADIANCE", 10) != 0) return fail(error, "HDR: no #?RADIANCE");
	if (size > static_cast<size_t>(std::numeric_limits<int32_t>::max())) return fail(error, "HDR: too large");
	// The header from the first byte (the magic's own line is skipped as any other):
	// a line starting "FORMAT=" names 32-bit_rle_rgbe or 32-bit_rle_xyze (decoded alike),
	// one starting "EXPOSURE=" multiplies the exposure, any other is skipped, and an
	// empty line ends it [orig: the loop @ 0x6DEAA1..0x6DEC93].
	size_t at = 0;
	int64_t remaining = static_cast<int64_t>(size);
	float exposure = 1.0f; // [orig: @ 0x6DEA94..0x6DEA98]
	bool format_found = false;
	const auto skip_blanks = [&]() {
		while (data[at] == ' ' || data[at] == '\t') {
			if (--remaining == 0) return false;
			++at;
		}
		return true;
	};
	for (;;) {
		if (data[at] == '\n') { // [orig: @ 0x6DEAA4, @ 0x6DEC90..0x6DEC93]
			++at;
			--remaining;
			break;
		}
		if (remaining > 7 && std::memcmp(data + at, "FORMAT=", 7) == 0) {
			// [orig: @ 0x6DEAAD..0x6DEB71 — 16 bytes stepped over: the name and one more]
			at += 7;
			remaining -= 7;
			if (!skip_blanks()) return fail(error, "HDR: the FORMAT line ends the data");
			if (remaining < 16) return fail(error, "HDR: the FORMAT line is short");
			if (std::memcmp(data + at, "32-bit_rle_rgbe", 15) != 0 &&
					std::memcmp(data + at, "32-bit_rle_xyze", 15) != 0)
				return fail(error, "HDR: an unknown FORMAT");
			at += 16;
			remaining -= 16;
			format_found = true;
		} else if (remaining > 9 && std::memcmp(data + at, "EXPOSURE=", 9) == 0) {
			// [orig: @ 0x6DEB7D..0x6DEC64 — the value within 256 bytes, "%f%s" == 1, the
			//  float product]
			at += 9;
			remaining -= 9;
			if (!skip_blanks()) return fail(error, "HDR: the EXPOSURE line ends the data");
			const size_t length = findendl(data + at, static_cast<size_t>(remaining >= 256 ? 256 : remaining));
			if (length == 0) return fail(error, "HDR: no EXPOSURE value");
			float value = 0.0f;
			if (!scan_f_only(copy_line(data + at, length), value)) return fail(error, "HDR: bad EXPOSURE");
			exposure = value * exposure;
			at += length + 1;
			remaining -= static_cast<int64_t>(length) + 1;
		} else {
			// [orig: @ 0x6DEC69..0x6DEC81] Retail steps one byte when the rest holds no
			// '\n'; nothing after can then be a resolution line, so the port fails there.
			const size_t length = findendl(data + at, static_cast<size_t>(remaining));
			if (length == 0) return fail(error, "HDR: a header line has no newline");
			at += length + 1;
			remaining -= static_cast<int64_t>(length) + 1;
		}
		if (remaining <= 0) break; // [orig: @ 0x6DEC84..0x6DEC88]
	}
	if (!format_found) return fail(error, "HDR: no FORMAT line"); // [orig: @ 0x6DEC96..0x6DEC9A]
	// The resolution line, longer than two bytes within the next 256
	// [orig: @ 0x6DECA0..0x6DECCE]: a sign and an axis, a number, then the first '-' or
	// '+' after it, an axis ('X' or 'Y', either) and a number
	// [orig: @ 0x6DECD3..0x6DEDCF]. "-Y" is top-down, "+Y" bottom-up, "+X" left to right,
	// "-X" right to left; the first axis steps from scanline to scanline, the second along
	// each one.
	const size_t limit = static_cast<size_t>(remaining > 256 ? 256 : remaining);
	const size_t length = findendl(data + at, limit);
	if (length <= 2) return fail(error, "HDR: no resolution line");
	const std::string line = copy_line(data + at, length);
	const auto ch = [&](size_t i) { return i < line.size() ? line[i] : '\0'; };
	bool first_y = false;
	bool y_plus = false;
	bool x_plus = false;
	if (ch(1) == 'Y')
		first_y = true;
	else if (ch(1) != 'X')
		return fail(error, "HDR: the resolution line names no axis");
	if (ch(0) != '-' && ch(0) != '+') return fail(error, "HDR: the resolution line has no sign");
	(first_y ? y_plus : x_plus) = ch(0) == '+';
	uint32_t first_count = 0;
	uint32_t second_count = 0;
	const char *p = line.c_str() + 2;
	if (!scan_u32(p, first_count)) return fail(error, "HDR: no first resolution number");
	size_t sign_at = 2;
	while (ch(sign_at) != '\0' && ch(sign_at) != '-' && ch(sign_at) != '+') ++sign_at;
	if (ch(sign_at) == '\0') return fail(error, "HDR: no second axis");
	(first_y ? x_plus : y_plus) = ch(sign_at) == '+';
	if (ch(sign_at + 1) != 'Y' && ch(sign_at + 1) != 'X') return fail(error, "HDR: no second axis letter");
	p = line.c_str() + sign_at + 2;
	if (!scan_u32(p, second_count)) return fail(error, "HDR: no second resolution number");
	at += length + 1;
	remaining -= static_cast<int64_t>(length) + 1;
	const uint32_t width = first_y ? second_count : first_count;
	const uint32_t height = first_y ? first_count : second_count;
	// Retail fails a 32-bit product of zero [orig: @ 0x6DEDF9..0x6DEE10] and wraps a huge
	// one; the port fails both, and a file whose scanlines cannot each start with a
	// 4-byte word, as its decode would.
	if (!pixel_count_ok(width, height)) return fail(error, "HDR: no pixels, or too many");
	const int64_t scanlines = first_y ? height : width;
	const int64_t scan_length = first_y ? width : height;
	if (scanlines * 4 > remaining) return fail(error, "HDR: the data cannot hold the scanlines");
	// The image is A32B32G32R32F [orig: format 116 @ 0x6DEE01]; the port holds each
	// pixel's R, G, B, E bytes (the integers the codec stores as floats), zeroed where
	// retail leaves its buffer unwritten (heap garbage there).
	const int64_t w = width;
	const int64_t h = height;
	out.width = static_cast<int>(width);
	out.height = static_cast<int>(height);
	out.pixels.assign(static_cast<size_t>(w * h) * 4u, 0);
	// Where each scanline starts and how it steps, in pixels [orig: @ 0x6DEE36..0x6DEED5].
	int64_t start = 0, inner = 0, outer = 0;
	if (first_y) {
		inner = x_plus ? 1 : -1;
		outer = y_plus ? -w : w;
		start = (y_plus ? (h - 1) * w : 0) + (x_plus ? 0 : w - 1);
	} else {
		outer = x_plus ? 1 : -1;
		inner = y_plus ? -w : w;
		start = (y_plus ? (h - 1) * w : 0) + (x_plus ? 0 : w - 1);
	}
	const int64_t total = w * h;
	// A write retail makes outside its buffer (only a negative old-style run can steer
	// one there) fails here.
	const auto put = [&](int64_t pixel, int channel, uint8_t value) {
		if (pixel < 0 || pixel >= total) return false;
		out.pixels[static_cast<size_t>(pixel) * 4u + static_cast<size_t>(channel)] = value;
		return true;
	};
	const auto put4 = [&](int64_t pixel, const uint8_t rgbe[4]) {
		for (int c = 0; c < 4; ++c)
			if (!put(pixel, c, rgbe[c])) return false;
		return true;
	};
	for (int64_t scanline = 0; scanline < scanlines; ++scanline) {
		const int64_t base = start + scanline * outer;
		if (remaining < 4) return fail(error, "HDR: the data ends before a scanline"); // [orig: @ 0x6DEEF4..0x6DEEF8]
		const uint8_t word[4] = {data[at], data[at + 1], data[at + 2], data[at + 3]};
		at += 4;
		remaining -= 4;
		if (word[0] == 2 || (word[1] == 2 && word[2] < 0x80)) {
			// New run-length: the word's last two bytes, big-endian, must be the scanline's
			// length [orig: @ 0x6DF0C6..0x6DF0D6]; then each of R, G, B, E in turn: a code
			// over 0x80 repeats the next byte (code & 0x7F) times, else copies `code` bytes,
			// a count past the length failing [orig: @ 0x6DF0DC..0x6DF1B6].
			if ((static_cast<int64_t>(word[2]) << 8) + word[3] != scan_length)
				return fail(error, "HDR: a run-length scanline's length is wrong");
			for (int channel = 0; channel < 4; ++channel) {
				int64_t count = 0;
				int64_t pixel = base;
				while (count < scan_length) {
					if (remaining < 2) return fail(error, "HDR: a run-length scanline is short");
					const uint8_t code = data[at];
					if (code > 0x80) {
						const int run = code & 0x7F;
						count += run;
						if (count > scan_length) return fail(error, "HDR: a run passes the scanline");
						const uint8_t value = data[at + 1];
						for (int i = 0; i < run; ++i, pixel += inner)
							if (!put(pixel, channel, value)) return fail(error, "HDR: a run leaves the image");
						at += 2;
						remaining -= 2;
					} else {
						if (remaining < static_cast<int64_t>(code) + 1)
							return fail(error, "HDR: a literal run is short");
						count += code;
						if (count > scan_length) return fail(error, "HDR: a literal run passes the scanline");
						for (int i = 0; i < code; ++i, pixel += inner)
							if (!put(pixel, channel, data[at + 1 + static_cast<size_t>(i)]))
								return fail(error, "HDR: a literal run leaves the image");
						at += static_cast<size_t>(code) + 1;
						remaining -= static_cast<int64_t>(code) + 1;
					}
				}
			}
			continue;
		}
		// Flat and old run-length: the first word is held, not written, and counts as the
		// scanline's first pixel; each later word is written where the scanline has got
		// to and becomes the held one, unless it is (1, 1, 1, n), which writes the held
		// one n << shift times, shift growing by 8 a run (x86's shl masks it to 5 bits)
		// and going back to 0 after a written word. So the scanline comes out one pixel
		// early and its last pixel is never written [orig: @ 0x6DEF26..0x6DF00A].
		uint8_t held[4] = {word[0], word[1], word[2], word[3]};
		uint32_t written = 1; // a signed count in retail, compared signed
		uint32_t shift = 0;
		int64_t pixel = base;
		const auto below = [&](uint32_t n) { return static_cast<int32_t>(n) < scan_length; };
		if (scan_length > 1) {
			do {
				if (remaining < 4) return fail(error, "HDR: a flat scanline is short");
				const uint8_t next[4] = {data[at], data[at + 1], data[at + 2], data[at + 3]};
				at += 4;
				remaining -= 4;
				if (next[0] == 1 && next[1] == 1 && next[2] == 1) {
					const uint32_t run = static_cast<uint32_t>(next[3]) << (shift & 31u);
					written += run;
					if (static_cast<int32_t>(written) > scan_length) return fail(error, "HDR: a run passes the scanline");
					for (int32_t i = 0; i < static_cast<int32_t>(run); ++i, pixel += inner)
						if (!put4(pixel, held)) return fail(error, "HDR: a run leaves the image");
					shift += 8;
				} else {
					shift = 0;
					std::memcpy(held, next, 4);
					if (!put4(pixel, held)) return fail(error, "HDR: a pixel leaves the image");
					++written;
					pixel += inner;
				}
			} while (below(written));
		}
	}
	// Every pixel, written or not: channel = (byte + 0.5) * 2^(E - 136) / exposure, no
	// special case for E == 0, rounded to a float; alpha 1.0
	// [orig: @ 0x6DF02A..0x6DF0BD — fld1 / exposure stored as a float @ 0x6DF03D..0x6DF045,
	//  _ftol of E @ 0x6DF04B, the 0.5 @ 0x7C3B94, CRT_ldexp @ 0x6DF065]. The scaled value
	// is exact in a double, so one rounding to float is the store's.
	float inverse;
	if (exposure == 0.0f)
		inverse = std::copysign(std::numeric_limits<float>::infinity(), exposure);
	else
		inverse = 1.0f / exposure;
	const auto scaled = [&](uint8_t byte, int exponent) {
		return round_to_float(std::ldexp(static_cast<double>(byte) + 0.5, exponent) * static_cast<double>(inverse));
	};
	for (size_t i = 0; i < static_cast<size_t>(total); ++i) {
		const uint8_t *px = out.pixels.data() + i * 4u;
		const int exponent = static_cast<int>(px[3]) - 136;
		const float r = scaled(px[0], exponent);
		const float g = scaled(px[1], exponent);
		const float b = scaled(px[2], exponent);
		put_float_rgb(out.pixels, i, r, g, b);
	}
	return true;
}

namespace {

// The header leg of the BMP codec's core: where it reads the pixels from, or false
// where it fails [orig: CImage_LoadBMP @ 0x6DB3F0, @ 0x6DB3FB..0x6DB979].
bool dib_pixel_offset(const uint8_t *data, size_t size, uint32_t &pixel_offset, std::string &error) {
	// [orig: @ 0x6DB3FB..0x6DB41A] Four bytes, and the header within the bytes.
	if (data == nullptr || size < 4) return fail(error, "DIB: under four bytes");
	const uint32_t header_size = io::read_u32_le(data);
	if (size < header_size) return fail(error, "DIB: the header passes the data");
	int32_t width = 0, height = 0;
	uint16_t planes = 0, bit_count = 0;
	uint32_t compression = 0, colors_used = 0, entry = 4;
	if (header_size == 12) {
		// BITMAPCOREHEADER: signed 16-bit sides, three-byte palette entries, no
		// compression [orig: @ 0x6DB42D..0x6DB46B].
		width = io::read_s16_le(data + 4);
		height = io::read_s16_le(data + 6);
		planes = io::read_u16_le(data + 8);
		bit_count = io::read_u16_le(data + 10);
		entry = 3;
	} else if (header_size < 40) {
		return fail(error, "DIB: a header under 40 bytes"); // [orig: @ 0x6DB470..0x6DB473]
	} else {
		width = io::read_s32_le(data + 4);
		height = io::read_s32_le(data + 8);
		planes = io::read_u16_le(data + 12);
		bit_count = io::read_u16_le(data + 14);
		compression = io::read_u32_le(data + 16);
		colors_used = io::read_u32_le(data + 32);
	}
	// [orig: @ 0x6DB47B..0x6DB482] The rows: the height's magnitude.
	const uint32_t rows = height <= 0 ? 0u - static_cast<uint32_t>(height) : static_cast<uint32_t>(height);
	// [orig: @ 0x6DB495..0x6DB4B8] The palette: biClrUsed entries, or 1 << bits for a
	// palettized image that names none; the pixels follow it (32-bit arithmetic).
	uint32_t colors = colors_used;
	if (bit_count <= 8 && colors == 0) colors = 1u << bit_count;
	uint32_t offset = header_size + entry * colors;
	if (offset > size) return fail(error, "DIB: the palette passes the data"); // [orig: @ 0x6DB4BB]
	if (planes != 1) return fail(error, "DIB: planes is not 1");               // [orig: @ 0x6DB4C1..0x6DB4C6]
	bool palettized = false;
	if (compression <= 2) {
		// BI_RGB, BI_RLE8, BI_RLE4: 1, 4, 8, 16, 24 or 32 bits [orig: @ 0x6DB81A..0x6DB848].
		switch (bit_count) {
			case 1:
			case 4:
			case 8:
				palettized = true;
				break;
			case 16:
			case 24:
			case 32:
				break;
			default:
				return fail(error, "DIB: an unknown bit count");
		}
	} else if (compression == 3) {
		// BI_BITFIELDS only with the masks inside a header of 52 bytes or more (never
		// after a 40-byte one), at 16, 24 or 32 bits [orig: @ 0x6DB4D8..0x6DB533]. Masks
		// red 0xFF000000, green 0xFF0000, blue 0xFF00, no alpha read as X8R8G8B8 one
		// byte later [orig: @ 0x6DB677..0x6DB6AB], the offset the size check uses.
		if (header_size < 52) return fail(error, "DIB: BI_BITFIELDS without the masks in the header");
		if (bit_count != 16 && bit_count != 24 && bit_count != 32) return fail(error, "DIB: an unknown bit count");
		const uint32_t red = io::read_u32_le(data + 40);
		const uint32_t green = io::read_u32_le(data + 44);
		const uint32_t blue = io::read_u32_le(data + 48);
		const uint32_t alpha = header_size >= 56 ? io::read_u32_le(data + 52) : 0u;
		if (bit_count == 32 && blue == 0xFF00u && green == 0xFF0000u && red == 0xFF000000u && alpha == 0) offset += 1;
	} else {
		return fail(error, "DIB: an unknown compression"); // [orig: @ 0x6DB4D8..0x6DB4DB]
	}
	// The port's bounds: retail writes past its 256-entry palette for more colours
	// [orig: @ 0x6DB8A9..0x6DB8DC], and makes an empty image of a zero side (whether the
	// texture is then made is not witnessed).
	if (palettized && colors > 256) return fail(error, "DIB: more than 256 palette entries");
	const uint32_t columns = static_cast<uint32_t>(width);
	if (!pixel_count_ok(columns, rows)) return fail(error, "DIB: no pixels, or too many");
	// [orig: @ 0x6DB911..0x6DB979] An uncompressed or bit-field image's rows, each
	// padded to four bytes, within the data.
	uint64_t row_bytes = 0;
	if (bit_count == 1)
		row_bytes = (uint64_t(columns) + 7u) >> 3;
	else if (bit_count == 4)
		row_bytes = (uint64_t(columns) + 1u) >> 1;
	else
		row_bytes = uint64_t(bit_count >> 3) * columns;
	const uint64_t stride = (row_bytes + 3u) & ~uint64_t(3);
	if ((compression == 0 || compression == 3) && uint64_t(offset) + row_bytes + stride * (rows - 1u) > size)
		return fail(error, "DIB: the pixel data is short");
	pixel_offset = offset;
	return true;
}

} // namespace

// [orig: D3DXTex::CImage::Load @ 0x6DF325..0x6DF32D — case 6 hands the bytes to
//  CImage_LoadBMP @ 0x6DB3F0 as they are]
bool d3dx_dib_to_bmp(const uint8_t *data, size_t size, std::vector<uint8_t> &bmp, std::string &error) {
	bmp.clear();
	uint32_t offset = 0;
	if (!dib_pixel_offset(data, size, offset, error)) return false;
	if (size > std::numeric_limits<uint32_t>::max() - 14u) return fail(error, "DIB: too large");
	bmp.reserve(size + 14u);
	bmp.push_back('B');
	bmp.push_back('M');
	io::append_u32_le(bmp, static_cast<uint32_t>(size + 14u)); // bfSize
	io::append_u32_le(bmp, 0);                                    // bfReserved1, bfReserved2
	io::append_u32_le(bmp, offset + 14u);                         // bfOffBits
	bmp.insert(bmp.end(), data, data + size);
	return true;
}

// [orig: D3DXTex::CImage::LoadBMP @ 0x6DE17B — 14 bytes, "BM", bfSize no more than the
//  bytes (@ 0x6DE183..0x6DE195), then CImage_LoadBMP on the bytes after the file header
//  (@ 0x6DE197..0x6DE19F); bfOffBits is never read]
bool d3dx_bmp_rehead(const uint8_t *data, size_t size, std::vector<uint8_t> &bmp, std::string &error) {
	bmp.clear();
	if (data == nullptr || size < 14) return fail(error, "BMP: under 14 bytes");
	if (data[0] != 'B' || data[1] != 'M') return fail(error, "BMP: no BM");
	if (io::read_u32_le(data + 2) > size) return fail(error, "BMP: bfSize passes the data");
	return d3dx_dib_to_bmp(data + 14, size - 14, bmp, error);
}

} // namespace opennova::renderer
