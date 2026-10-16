// Retail terrain TGAs through the carried form: every colour map (_c.tga), detail blend map (_d1.tga) and tile
// atlas (trntile*.tga) directly under the extracted asset tree reads a form (each ends with the TGA 2.0 footer),
// and decoded the way the game's reader decodes it and written again in that form, is the same bytes. Then every
// TGA anywhere under the tree that the editor paints as a plane (a form tga_read_form reads,
// editor/documents/texture_raster.cpp read_tga) is written again as the same bytes: the raster core's claim that a
// file read and written again is its bytes. Reports Skipped without OPENNOVA_JO_ASSETS.

#include <formats/tga/tga.h>
#include <formats/tga/tga_read.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include "common/file_io.h"
#include "common/retail_paths.h"

namespace {

std::string lower(std::string s) {
	for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

bool ends_with(const std::string &s, const char *tail) {
	const std::string t(tail);
	return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
}

} // namespace

int main() {
	const std::string assets = retail::assets();
	if (assets.empty())
		return retail::skip("OPENNOVA_JO_ASSETS (the extracted retail tree with its loose terrain .tga maps)");
	int failures = 0, colour = 0, blend = 0, atlas = 0;
	for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(assets)) {
		if (!entry.is_regular_file()) continue;
		const std::string name = entry.path().filename().string();
		const std::string l = lower(name);
		const bool is_colour = ends_with(l, "_c.tga"), is_blend = ends_with(l, "_d1.tga");
		const bool is_atlas = l.rfind("trntile", 0) == 0 && ends_with(l, ".tga");
		if (!is_colour && !is_blend && !is_atlas) continue;
		const std::vector<uint8_t> bytes = test_io::read_file(entry.path().string());
		opennova::tga::TgaForm form;
		opennova::tga::TgaImage image;
		std::string error;
		if (!opennova::tga::tga_read_form(bytes.data(), bytes.size(), form) || !form.footer) {
			std::printf("FAIL: %s: no form with the TGA 2.0 footer\n", name.c_str());
			++failures;
			continue;
		}
		colour += is_colour, blend += is_blend, atlas += is_atlas;
		std::vector<uint8_t> again;
		if (!opennova::tga::tga_decode_retail(bytes.data(), bytes.size(), image, error) ||
		    !opennova::tga::tga_write(image.rgba.data(), uint32_t(image.width), uint32_t(image.height), form, again,
		                              error) ||
		    again != bytes) {
			std::printf("FAIL: %s written again in its form differs (%zu bytes, written %zu)\n", name.c_str(),
			            bytes.size(), again.size());
			++failures;
		}
	}
	if (colour == 0 || blend == 0 || atlas == 0) {
		std::printf("FAIL: no _c.tga, _d1.tga and trntile*.tga under %s\n", assets.c_str());
		return 1;
	}
	// Every paintable TGA in the tree, the terrain maps among them.
	int paintable = 0, whole = 0;
	for (const std::filesystem::directory_entry &entry : std::filesystem::recursive_directory_iterator(assets)) {
		if (!entry.is_regular_file() || lower(entry.path().extension().string()) != ".tga") continue;
		const std::vector<uint8_t> bytes = test_io::read_file(entry.path().string());
		opennova::tga::TgaForm form;
		opennova::tga::TgaImage image;
		std::string error;
		if (!opennova::tga::tga_read_form(bytes.data(), bytes.size(), form)) {
			++whole;
			continue;
		}
		++paintable;
		std::vector<uint8_t> again;
		if (!opennova::tga::tga_decode_retail(bytes.data(), bytes.size(), image, error) ||
		    !opennova::tga::tga_write(image.rgba.data(), uint32_t(image.width), uint32_t(image.height), form, again,
		                              error) ||
		    again != bytes) {
			std::printf("FAIL: %s written again in its form differs (%zu bytes, written %zu)\n",
			            entry.path().string().c_str(), bytes.size(), again.size());
			++failures;
		}
	}
	if (paintable < colour + blend + atlas) {
		std::printf("FAIL: %d paintable TGA in the tree, fewer than its %d terrain maps\n", paintable,
		            colour + blend + atlas);
		return 1;
	}
	std::printf("%s: %d colour maps, %d detail blend maps, %d tile atlases; %d paintable TGA in the tree written "
	            "again (%d held whole), %d failures\n",
	            failures == 0 ? "OK" : "FAIL", colour, blend, atlas, paintable, whole, failures);
	return failures == 0 ? 0 : 1;
}
