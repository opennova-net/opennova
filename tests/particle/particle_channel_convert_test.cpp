// The witnessed atlas-time channel conversions
// [orig: Texture_GenerateNormalMapFromHeight @ 0x5e2df0; the type-1 alpha
// clear in CParticleManager_BuildTextureAtlases @ 0x5e9116]: the blue byte is
// height, the gradient sign pair is b(x-1)-b(x+1) / b(x,y+1)-b(x,y-1),
// neighbors wrap with power-of-two masks, encoding is (n+1)*127.5, alpha is
// untouched by the normal conversion, the type-7 form forces blue to 255, and
// the additive clear zeroes every alpha byte.

#include <runtime/particle/channel_convert.h>

#include <cstdio>
#include <vector>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

bool test_flat_height_is_unit_up() {
	// Uniform height -> zero gradient -> normal (0,0,1): encoded (127,127,255).
	std::vector<std::uint8_t> rgba(4 * 4 * 4);
	for (std::size_t i = 0; i < rgba.size(); i += 4) {
		rgba[i + 0] = 10;
		rgba[i + 1] = 20;
		rgba[i + 2] = 90;
		rgba[i + 3] = 77;
	}
	opennova::particle::convert_height_to_normal_map(rgba.data(), 4, 4,
			opennova::particle::kBumpHeightToNormalScale, false);
	bool ok = true;
	ok &= expect(rgba[0] == 127, "flat nx encodes 127");
	ok &= expect(rgba[1] == 127, "flat ny encodes 127");
	ok &= expect(rgba[2] == 255, "flat nz encodes 255");
	ok &= expect(rgba[3] == 77, "alpha is untouched");
	return ok;
}

bool test_gradient_signs_and_wrap() {
	// One bright column at x=1 on a 4-wide strip. At x=0 the witnessed
	// gradient is b(x-1)-b(x+1) = b(3)-b(1) = -255*scale (negative nx);
	// at x=2 it is b(1)-b(3) = +255*scale.
	std::vector<std::uint8_t> rgba(4 * 1 * 4, 0);
	rgba[1 * 4 + 2] = 255;
	opennova::particle::convert_height_to_normal_map(rgba.data(), 4, 1,
			opennova::particle::kBumpHeightToNormalScale, false);
	bool ok = true;
	ok &= expect(rgba[0 * 4 + 0] < 127, "negative nx encodes below midpoint");
	ok &= expect(rgba[2 * 4 + 0] > 127, "positive nx encodes above midpoint");
	// x=3 sees b(2)-b(0) = 0: the wrap keeps it flat.
	ok &= expect(rgba[3 * 4 + 0] == 127, "wrapped neighbor keeps x=3 flat");
	return ok;
}

bool test_distort_forces_blue() {
	std::vector<std::uint8_t> rgba(2 * 2 * 4, 0);
	rgba[2] = 200;
	rgba[3] = 33;
	opennova::particle::convert_height_to_normal_map(rgba.data(), 2, 2,
			opennova::particle::kDistortHeightToNormalScale, true);
	bool ok = true;
	for (int i = 0; i < 4; ++i)
		ok &= expect(rgba[i * 4 + 2] == 255, "type-7 blue forced to 255");
	ok &= expect(rgba[3] == 33, "type-7 alpha is untouched");
	return ok;
}

bool test_additive_alpha_clear() {
	std::vector<std::uint8_t> rgba(2 * 2 * 4, 200);
	opennova::particle::clear_alpha_channel(rgba.data(), 2, 2);
	bool ok = true;
	for (int i = 0; i < 4; ++i) {
		ok &= expect(rgba[i * 4 + 3] == 0, "type-1 alpha cleared");
		ok &= expect(rgba[i * 4 + 0] == 200, "type-1 rgb untouched");
	}
	return ok;
}

} // namespace

int main() {
	bool ok = true;
	ok &= test_flat_height_is_unit_up();
	ok &= test_gradient_signs_and_wrap();
	ok &= test_distort_forces_blue();
	ok &= test_additive_alpha_clear();
	return ok ? 0 : 1;
}
