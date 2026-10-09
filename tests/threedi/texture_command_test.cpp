// opennova-3di texture (apps/3di/texture.cpp) over the handler the CLI dispatches: an image written as a
// model's .dds (auto DXT1 or DXT5, the full chain to 1 x 1 or one level, A8R8G8B8), .tga or .mdt, halved
// to a cap; a .dds whose sides are not powers of two refused, and the flags a .tga does not take; the
// authoring encoder's blocks kept at least as close to the source as the D3DX codec's port keeps them.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <formats/dds/dds.h>
#include <formats/tga/tga.h>
#include <runtime/renderer/dxt_encode.h>
#include <runtime/renderer/texture_dxt.h>

#include "../../apps/3di/threedi_cli.h"
#include "../common/file_io.h"

namespace threedi_cli = opennova::threedi_cli;
namespace dds = opennova::dds;
namespace renderer = opennova::renderer;

namespace {
int failures = 0;
void check(bool ok, const char *what) {
	if (!ok) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++failures;
	}
}

// A smooth image with detail: colour ramps across it, a ripple, and an alpha that is `alpha` where set and a
// ramp otherwise.
std::vector<uint8_t> picture(uint32_t w, uint32_t h, int alpha) {
	std::vector<uint8_t> rgba(size_t(w) * h * 4);
	for (uint32_t y = 0; y < h; ++y)
		for (uint32_t x = 0; x < w; ++x) {
			uint8_t *p = &rgba[(size_t(y) * w + x) * 4];
			p[0] = uint8_t(x * 255 / std::max(1u, w - 1));
			p[1] = uint8_t(y * 255 / std::max(1u, h - 1));
			p[2] = uint8_t(128 + 100 * std::sin(double(x + y) * 0.3));
			p[3] = alpha >= 0 ? uint8_t(alpha) : uint8_t((x * 7 + y * 3) & 0xFF);
		}
	return rgba;
}

std::string write_tga(const std::string &dir, const std::string &name, const std::vector<uint8_t> &rgba, uint32_t w, uint32_t h) {
	std::vector<uint8_t> bytes;
	std::string why;
	opennova::tga::tga_write_rgba32(rgba.data(), w, h, bytes, why);
	const std::string path = dir + "/" + name;
	test_io::write_file(path, bytes);
	return path;
}

int run(const std::string &in, const std::string &out, const std::string &format = "", const std::string &mips = "",
        uint32_t max_size = 0, const std::string &alpha = "") {
	threedi_cli::TextureCommand command;
	command.input = in;
	command.output = out;
	command.format = format;
	command.mips = mips;
	command.max_size = max_size;
	command.alpha = alpha;
	return threedi_cli::cmd_texture(command);
}

bool read_dds(const std::string &path, dds::DdsImage &image) {
	std::vector<uint8_t> bytes;
	std::string error;
	return test_io::read_file(path, bytes) && dds::dds_read(bytes.data(), bytes.size(), image, error) && image.loads;
}

double psnr_rgb(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) {
	double se = 0;
	for (size_t i = 0; i < a.size(); ++i)
		if (i % 4 != 3) se += (double(a[i]) - double(b[i])) * (double(a[i]) - double(b[i]));
	const double mse = se / (double(a.size()) * 0.75);
	return mse == 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

std::vector<uint8_t> decoded(const std::vector<uint8_t> &blocks, uint32_t w, uint32_t h, renderer::TextureDxtFormat format) {
	renderer::DxtSurface surface;
	surface.format = format;
	surface.width = w;
	surface.height = h;
	surface.blocks = blocks;
	return renderer::encode_rgba8(renderer::decode_dxt_surface(surface));
}

} // namespace

int main(int argc, char **argv) {
	const std::string dir = (argc > 1 ? std::string(argv[1]) : std::string(".")) + "/texture_command";
	std::filesystem::create_directories(dir);

	// An opaque image: DXT1, every level to 1 x 1 (one per halving of the larger side).
	const std::string opaque = write_tga(dir, "opaque.tga", picture(64, 32, 255), 64, 32);
	dds::DdsImage image;
	check(run(opaque, dir + "/opaque.dds") == 0, "an opaque image writes");
	check(read_dds(dir + "/opaque.dds", image) && std::string(image.format.name) == "DXT1" && image.levels.size() == 7 &&
	              image.levels.back().width == 1 && image.levels.back().height == 1,
	      "auto writes an opaque image as DXT1 with its chain to 1 x 1");
	// Graded alpha: DXT5; one level on asking.
	const std::string graded = write_tga(dir, "graded.tga", picture(32, 32, -1), 32, 32);
	check(run(graded, dir + "/graded.dds", "", "none") == 0 && read_dds(dir + "/graded.dds", image) &&
	              std::string(image.format.name) == "DXT5" && image.levels.size() == 1,
	      "auto writes an image with alpha as DXT5, one level with --mips none");
	check(run(graded, dir + "/argb.dds", "argb") == 0 && read_dds(dir + "/argb.dds", image) &&
	              std::string(image.format.name) == "A8R8G8B8" && image.levels.size() == 1 && image.levels[0].rgba == picture(32, 32, -1),
	      "argb writes the texels as they are");
	// Its alpha made opaque (a stray alpha no shader reads): DXT1 by the auto rule.
	check(run(graded, dir + "/flat.dds", "", "", 0, "opaque") == 0 && read_dds(dir + "/flat.dds", image) &&
	              std::string(image.format.name) == "DXT1",
	      "--alpha opaque writes an image with alpha as DXT1");
	check(run(graded, dir + "/bad.dds", "", "", 0, "half") == 2, "--alpha takes the import's alpha forms");
	check(run(opaque, dir + "/forced.dds", "dxt5") == 0 && read_dds(dir + "/forced.dds", image) &&
	              std::string(image.format.name) == "DXT5",
	      "--format dxt5 is DXT5 whatever the alpha");
	// Halved to a cap, the 2 x 2 box as the game halves.
	check(run(opaque, dir + "/capped.dds", "", "", 16) == 0 && read_dds(dir + "/capped.dds", image) &&
	              image.header.width == 16 && image.header.height == 8 && image.levels.size() == 5,
	      "--max-size halves while a side exceeds it");
	// A .tga and an .mdt: 32-bit TGAs, halved alike; an .mdt read back as a source.
	std::vector<uint8_t> bytes;
	opennova::tga::TgaHeader tga;
	check(run(graded, dir + "/half.mdt", "", "", 16) == 0 && test_io::read_file(dir + "/half.mdt", bytes) &&
	              opennova::tga::tga_read_header(bytes.data(), bytes.size(), tga) && tga.width == 16 && tga.height == 16 &&
	              tga.bits == 32,
	      "an .mdt is a 32-bit TGA, halved to the cap");
	check(run(dir + "/half.mdt", dir + "/again.tga") == 0 && test_io::read_file(dir + "/again.tga", bytes) &&
	              opennova::tga::tga_read_header(bytes.data(), bytes.size(), tga) && tga.width == 16,
	      "an .mdt reads as the TGA it is");
	// Refusals: a .dds of sides that are no powers of two (the game pads them), nothing written; the flags a
	// .tga does not take; a format no row has; an output of another extension.
	const std::string odd = write_tga(dir, "odd.tga", picture(48, 32, 255), 48, 32);
	check(run(odd, dir + "/odd.dds") == 1 && !std::filesystem::exists(dir + "/odd.dds"),
	      "a .dds whose sides are not powers of two is refused");
	check(run(odd, dir + "/odd.tga.out.tga") == 0, "a .tga of any sides writes");
	check(run(opaque, dir + "/flag.tga", "dxt1") == 2, "--format is a .dds's");
	check(run(opaque, dir + "/bad.dds", "dxt3") == 2, "--format takes auto, dxt1, dxt5 or argb");
	check(run(opaque, dir + "/bad.dds", "argb", "full") == 2, "an argb .dds takes no --mips");
	check(run(opaque, dir + "/out.png") == 2, "texture writes a .dds, a .tga or an .mdt");
	check(run(dir + "/missing.tga", dir + "/missing.dds") == 1, "a missing source fails");

	// The authoring encoder against the D3DX codec's port, level 0 of the same picture: at least as close.
	{
		const std::vector<uint8_t> source = picture(128, 128, 255);
		const auto chain = renderer::encode_dxt_levels(source.data(), 128, 128, false, true);
		check(chain.size() == 8, "a 128 x 128 chain has eight levels");
		const renderer::DxtSurface port = renderer::encode_dxt_surface(renderer::decode_rgba8(source.data(), 128, 128), 128, 128,
		                                                               renderer::TextureDxtFormat::Dxt1);
		const double ours = psnr_rgb(source, decoded(chain[0], 128, 128, renderer::TextureDxtFormat::Dxt1));
		const double theirs = psnr_rgb(source, decoded(port.blocks, 128, 128, renderer::TextureDxtFormat::Dxt1));
		std::printf("DXT1 level 0: authoring encoder %.2f dB, the D3DX codec's port %.2f dB\n", ours, theirs);
		check(ours >= theirs && ours > 32.0, "the authoring encoder keeps DXT1 at least as close as the port");
		const std::vector<uint8_t> alpha = picture(128, 128, -1);
		const auto five = renderer::encode_dxt_levels(alpha.data(), 128, 128, true, false);
		const renderer::DxtSurface port5 = renderer::encode_dxt_surface(renderer::decode_rgba8(alpha.data(), 128, 128), 128, 128,
		                                                                renderer::TextureDxtFormat::Dxt5);
		const double ours5 = psnr_rgb(alpha, decoded(five[0], 128, 128, renderer::TextureDxtFormat::Dxt5));
		const double theirs5 = psnr_rgb(alpha, decoded(port5.blocks, 128, 128, renderer::TextureDxtFormat::Dxt5));
		std::printf("DXT5 level 0: authoring encoder %.2f dB, the D3DX codec's port %.2f dB\n", ours5, theirs5);
		check(five.size() == 1 && ours5 >= theirs5, "the authoring encoder keeps DXT5 at least as close as the port");
	}
	// A DXT1 texture keeps its alpha on or off: a block holding a clear texel is colour-keyed, the clear texels
	// decoded transparent, the opaque ones opaque.
	{
		std::vector<uint8_t> keyed = picture(8, 8, 255);
		for (size_t i = 0; i < 8; ++i) keyed[i * 4 + 3] = 0; // the first row clear
		const auto chain = renderer::encode_dxt_levels(keyed.data(), 8, 8, false, false);
		const std::vector<uint8_t> back = decoded(chain[0], 8, 8, renderer::TextureDxtFormat::Dxt1);
		bool clear = true, solid = true;
		for (size_t i = 0; i < 64; ++i) (i < 8 ? clear : solid) = (i < 8 ? clear : solid) && back[i * 4 + 3] == (i < 8 ? 0 : 255);
		check(clear && solid, "a DXT1 block's clear texels stay clear");
	}

	if (failures == 0) std::printf("texture_command: all checks passed\n");
	return failures == 0 ? 0 : 1;
}
