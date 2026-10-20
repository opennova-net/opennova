// Every committed .3di model through the engine's one reader and writer: read
// -> write gives the file's own bytes, and a second read -> write gives the
// first write's bytes. The editor saves a model with exactly this writer (ADR
// 0046 S10), so a model opened and saved untouched must come back as the file.
// A mismatch names the first chunk whose bytes differ. The models are
// fixtures/threedi (the synthetic set and the variants minted beside it,
// tests/fixtures/minimal_3di_gen.cpp), which stand for the 958 shipped models
// this row read until 2026-10-19 (each shipped back as its own bytes). Each write is pinned by its
// FNV-1a64 below: a vector that moves is a change to the bytes the editor
// saves, and needs an [orig] witness or a D-entry in the same commit
// (`threedi_rewrite_test --dump` prints the table).
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <formats/threedi/threedi.h>
#include <formats/threedi/threedi_3di3.h>

#include "common/file_io.h"
#include "common/test_paths.h"

using namespace opennova::threedi;

namespace {

uint64_t fnv1a64(const std::vector<uint8_t> &bytes) {
	uint64_t h = 1469598103934665603ull;
	for (uint8_t b : bytes) {
		h ^= b;
		h *= 1099511628211ull;
	}
	return h;
}

struct Vector {
	const char *name;
	size_t len;
	uint64_t hash;
};

// The write of every model, in path order under fixtures/threedi.
const Vector kExpected[] = {
		{"crate_alphagen24.3di", 3256u, 0x222497a7d650a105ull},
		{"synth/armory.3di", 19860u, 0xf21c234b364984acull},
		{"synth/armory_lght0_colorgen113_flicker.3di", 19860u, 0x2cede5460fa1456full},
		{"synth/armory_lght0_sub1_offset.3di", 19860u, 0x4919f1e653af23ebull},
		{"synth/armory_special1_slide_part1.3di", 19656u, 0xef8821e72cb7cfc7ull},
		{"synth/armory_special2_slide_part1.3di", 19656u, 0x0772a81b135c22eeull},
		{"synth/bird.3di", 17660u, 0x34970eb4b6b9d398ull},
		{"synth/carrier.3di", 7556u, 0x2b6e13d4f8235fdaull},
		{"synth/crate.3di", 3256u, 0x97b4b35f11581719ull},
		{"synth/crate_mtrl0_ab_lum_upl113.3di", 3280u, 0xa359fad884887b6cull},
		{"synth/crate_mtrl0_ad_lum_upl113.3di", 3280u, 0xbbf62d497efdb0feull},
		{"synth/gun.3di", 3764u, 0x9fd115f585aaf2f8ull},
		{"synth/heli.3di", 20736u, 0x9728212e02e2aacaull},
		{"synth/house.3di", 6956u, 0xf5104533c18c7012ull},
		{"synth/house_lod0_sine_rotx.3di", 7024u, 0x60d5aaa0a447449aull},
		{"synth/house_lod0_sine_rotx_uv1.3di", 7024u, 0x9722a8353fe4528bull},
		{"synth/house_mtrl0_uvscroll16_alphatest.3di", 6956u, 0x51dad17c2badfaa2ull},
		{"synth/mount.3di", 12072u, 0x78051f487c81a5e4ull},
		{"synth/mount_ctrl1_heat_glow.3di", 12072u, 0x2f16cb36722072d0ull},
		{"synth/mount_ctrl1_lod_frac_yaw_style114.3di", 12072u, 0x22f067e4fb8a6cc5ull},
		{"synth/mount_ctrl1_not_retail.3di", 12072u, 0x55df26a6ccba54dfull},
		{"synth/mount_heat_glow_slide_part1.3di", 11936u, 0xfab5504616c36bd9ull},
		{"synth/mount_mtrl0_rgbgen113_reg1.3di", 12072u, 0x0aae6517c23993b8ull},
		{"synth/mount_mtrl2_ab_lum.3di", 12072u, 0xfa7c4c32df180f4aull},
		{"synth/mount_yaw_style114.3di", 12072u, 0xbc975d6848c0c773ull},
		{"synth/panm_inert_05_rot_scalex.3di", 5168u, 0x93ff61a28501c664ull},
		{"synth/panm_inert_06_uniform_scaley.3di", 5168u, 0xe9e184760ee45117ull},
		{"synth/panm_inert_10_rotrev.3di", 5168u, 0x3500d327e038555full},
		{"synth/panm_live_01_spinner.3di", 5168u, 0x5f31360c353bdeb1ull},
		{"synth/panm_live_02_view3.3di", 5168u, 0x6cc110fd0c065eabull},
		{"synth/panm_live_03_view4.3di", 5168u, 0xe97f523c971b7f96ull},
		{"synth/panm_live_04_rotz.3di", 5168u, 0x5a4210f408744464ull},
		{"synth/panm_live_07_uniform_scalex.3di", 5168u, 0x648cf5e497930f17ull},
		{"synth/panm_live_08_axis_scaley.3di", 5168u, 0x8001c94a9b611458ull},
		{"synth/panm_live_09_translation.3di", 5168u, 0x230bfdf46272ba29ull},
		{"synth/person.3di", 34332u, 0xb3777b26bae17491ull},
		{"synth/person_mtrl0_ad_lum.3di", 34332u, 0xd16ae75d3c2ad544ull},
		{"synth/person_part9_trigger_scale.3di", 34356u, 0x4239c06879b11243ull},
		{"synth/person_rest4.3di", 45276u, 0x7832c15b5cef6b0cull},
		{"synth/person_slot2.3di", 45276u, 0xf7a78900819d4cdcull},
		{"synth/pump.3di", 18996u, 0xad8e3dc7e6aa2d85ull},
		{"synth/pump_anim0_noise_translation.3di", 18996u, 0x534cf46b92aa4f8dull},
		{"synth/pump_lod0_inert_lod1_sine_rotz.3di", 18452u, 0x33f4097dc5129a18ull},
		{"synth/pump_lod20.3di", 18996u, 0x3550234a9fa45469ull},
		{"synth/pump_lod80.3di", 18996u, 0x407e8e38bdc9b9b5ull},
		{"synth/pump_minefield.3di", 19812u, 0x11c811966c90f156ull},
		{"synth/pump_mtrl1_mt_alphatest.3di", 18996u, 0x9c612f0667fda13cull},
		{"synth/shed.3di", 5168u, 0x999a96e9ee9a0bf2ull},
		{"synth/shed_lght0_sub2_origin_atten100.3di", 5168u, 0x9ca3437021f1b44bull},
		{"synth/tank.3di", 9560u, 0x5f2bccfb81ba55f4ull},
		{"synth/tank_special1_slide_ewep01.3di", 9492u, 0x22433bf0787b8099ull},
		{"synth/vm40.3di", 74548u, 0x0f79a7034fef281bull},
};

// The path of the first chunk whose id, size or payload differs ("" when the
// trees match), e.g. "RLOD[1]/PANM".
std::string first_difference(const ThreediChunk &a, const ThreediChunk &b, const std::string &at) {
	const std::string here = at.empty() ? std::string(a.id) : at + "/" + a.id;
	if (std::strcmp(a.id, b.id) != 0) return here + " (id " + a.id + " vs " + b.id + ")";
	if (a.is_parent != b.is_parent || a.child_count != b.child_count)
		return here + " (children " + std::to_string(a.child_count) + " vs " + std::to_string(b.child_count) + ")";
	if (!a.is_parent) {
		if (a.data_len != b.data_len)
			return here + " (size " + std::to_string(a.data_len) + " vs " + std::to_string(b.data_len) + ")";
		for (size_t i = 0; i < a.data_len; ++i)
			if (a.data[i] != b.data[i]) return here + " (byte " + std::to_string(i) + ")";
		return std::string();
	}
	for (size_t i = 0; i < a.child_count; ++i) {
		const std::string child = first_difference(a.children[i], b.children[i],
				here + "[" + std::to_string(i) + "]");
		if (!child.empty()) return child;
	}
	return std::string();
}

std::string describe(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) {
	ThreediFile fa = {}, fb = {};
	std::string where = "the chunk trees";
	if (threedi_read_memory(a.data(), a.size(), &fa) == 0 && threedi_read_memory(b.data(), b.size(), &fb) == 0) {
		if (fa.version != fb.version) where = "the version word";
		else where = first_difference(*fa.root, *fb.root, "");
		if (where.empty()) where = "bytes outside the chunks";
	}
	threedi_free_file(&fa);
	threedi_free_file(&fb);
	return where;
}

// Read, write; "" and the written bytes, or what failed.
std::string rewrite(const std::vector<uint8_t> &in, std::vector<uint8_t> &out) {
	Threedi3di3 model = {};
	if (threedi_3di3_read_memory(in.data(), in.size(), &model) != 0) return "does not read";
	const int rc = threedi_3di3_write_memory(&model, out);
	threedi_3di3_free(&model);
	return rc == 0 ? std::string() : std::string("does not write");
}

} // namespace

int main(int argc, char **argv) {
	const bool dump = argc > 1 && std::strcmp(argv[1], "--dump") == 0;
	const std::filesystem::path root = test_paths_fixture("threedi");
	std::vector<std::string> models;
	std::error_code ec;
	for (const auto &entry : std::filesystem::recursive_directory_iterator(root, ec)) {
		if (!entry.is_regular_file(ec)) continue;
		std::string ext = entry.path().extension().string();
		for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		if (ext == ".3di") models.push_back(entry.path().lexically_relative(root).generic_string());
	}
	std::sort(models.begin(), models.end());

	int failures = 0;
	std::vector<Vector> written;
	for (const std::string &name : models) {
		const std::vector<uint8_t> original = test_io::read_file((root / name).string());
		std::vector<uint8_t> first, second;
		std::string problem = test_io::is_lfs_pointer(original) ? "is an unpulled LFS pointer" : rewrite(original, first);
		if (problem.empty() && first != original) problem = "rewrites " + describe(original, first);
		if (problem.empty()) problem = rewrite(first, second);
		if (problem.empty() && second != first) problem = "is not a fixed point at " + describe(first, second);
		if (!problem.empty()) {
			std::printf("FAIL %s: %s\n", name.c_str(), problem.c_str());
			++failures;
		}
		written.push_back(Vector{nullptr, first.size(), fnv1a64(first)});
	}
	if (dump) {
		std::printf("const Vector kExpected[] = {\n");
		for (size_t i = 0; i < models.size(); ++i)
			std::printf("\t\t{\"%s\", %zuu, 0x%016llxull},\n", models[i].c_str(), written[i].len,
					static_cast<unsigned long long>(written[i].hash));
		std::printf("};\n");
		return 0;
	}
	const size_t expected = sizeof(kExpected) / sizeof(kExpected[0]);
	if (expected != models.size()) {
		std::printf("FAIL: fixtures/threedi holds %zu models, %zu vectors are committed (--dump)\n", models.size(), expected);
		++failures;
	}
	for (size_t i = 0; i < models.size() && i < expected; ++i)
		if (models[i] != kExpected[i].name || written[i].len != kExpected[i].len || written[i].hash != kExpected[i].hash) {
			std::printf("FAIL %s: writes %zu bytes hash 0x%016llx, pinned %s %zu bytes 0x%016llx\n", models[i].c_str(),
					written[i].len, static_cast<unsigned long long>(written[i].hash), kExpected[i].name,
					kExpected[i].len, static_cast<unsigned long long>(kExpected[i].hash));
			++failures;
		}
	std::printf("%zu models, %d rewritten differently or unpinned\n", models.size(), failures);
	return failures == 0 ? 0 : 1;
}
