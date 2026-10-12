// Painting a texture (ADR 0046 S25 B3): the texture document on the raster core. An 8-bit PCX (the shipped maps'
// coder and JO's mission editor's), an uncompressed TGA (24 and 32 bits, with and without the TGA 2.0 footer) and a
// true-colour PNG read into one plane beside their form, written again from it as the same bytes; a DDS, a
// run-length TGA and a palette PNG held whole, no brush. Texels painted are one step (a stroke's batches one), their
// tiles a region of the document's changes, undone to the file as read; a texture operation (S18) is a whole change.
// The retail twin paints every shipped char, foliage, colour and blend map at one texel, undoes it and saves it
// identical, and writes each again from its plane as the same bytes.
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <editor/documents/texture_document.h>
#include <editor/documents/texture_operations.h>
#include <editor/documents/texture_raster.h>
#include <formats/dds/dds.h>
#include <formats/pcx/pcx_io.h>
#include <formats/png/png_encode.h>
#include <formats/tga/tga.h>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"

using namespace opennova;
using namespace opennova::editor;

namespace {

std::unique_ptr<TextureDocument> open(const std::string &name, const std::vector<uint8_t> &bytes) {
	auto document = std::make_unique<TextureDocument>();
	Diagnostic error;
	if (!document->load_bytes(bytes, name, AssetKind::Texture, "jo", error)) return nullptr;
	return document;
}

std::vector<uint8_t> bytes_of(const TextureDocument &document) {
	const SerializeResult saved = document.serialize();
	return std::vector<uint8_t>(saved.text.begin(), saved.text.end());
}

// One texel of the plane painted `value` (its texel bytes).
Edit paint(const TextureDocument &document, int x, int y, std::vector<uint8_t> value, uint64_t gesture = 0) {
	const RasterPlane &plane = document.plane(0);
	auto edit = std::make_shared<TexturePaintEdit>();
	edit->format = plane.format();
	edit->width = plane.width();
	edit->height = plane.height();
	edit->at = {uint32_t(y) * uint32_t(plane.width()) + uint32_t(x)};
	edit->values = std::move(value);
	return RasterDocument::edit(edit, gesture);
}

IndexedImage8 indexed(int width, int height) {
	IndexedImage8 image;
	image.width = width;
	image.height = height;
	for (int i = 0; i < width * height; ++i) image.indices.push_back(uint8_t(i % 7 == 0 ? 3 : i / 13));
	for (int i = 0; i < 256; ++i) {
		image.palette[i][0] = uint8_t(i);
		image.palette[i][1] = uint8_t(255 - i);
		image.palette[i][2] = uint8_t(i * 3);
	}
	return image;
}

std::vector<uint8_t> rgba(int width, int height) {
	std::vector<uint8_t> out;
	for (int i = 0; i < width * height; ++i) {
		out.push_back(uint8_t(i * 5));
		out.push_back(uint8_t(i * 3 + 1));
		out.push_back(uint8_t(255 - i));
		out.push_back(uint8_t(i % 2 ? 255 : 128));
	}
	return out;
}

// The file read into its plane and written again from it is the file.
bool written_again(const std::string &name, const std::vector<uint8_t> &bytes) {
	std::vector<uint8_t> again;
	std::string error;
	return write_texture_state(read_texture_state(name, bytes), again, error) && again == bytes;
}

int test_forms() {
	std::string why;
	// An 8-bit PCX in the shipped maps' coder and in JO's mission editor's.
	std::vector<uint8_t> shipped, med;
	TEST_EXPECT(encode_pcx_indexed(indexed(70, 9), shipped, why));
	const PcxIndexedForm med_form = pcx_med_form();
	TEST_EXPECT(encode_pcx_indexed(indexed(70, 9), med, why, &med_form));
	TEST_EXPECT(written_again("a_m.pcx", shipped) && written_again("a_m.pcx", med));
	// An uncompressed TGA, 24 bits with the footer and 32 without.
	std::vector<uint8_t> tga24, tga32;
	tga::TgaForm footer;
	footer.bits = 24;
	footer.descriptor = 0;
	footer.footer = true;
	TEST_EXPECT(tga::tga_write(rgba(5, 3).data(), 5, 3, footer, tga24, why));
	TEST_EXPECT(tga::tga_write_rgba32(rgba(5, 3).data(), 5, 3, tga32, why));
	TEST_EXPECT(written_again("a_c.tga", tga24) && written_again("a_d1.tga", tga32));
	const auto pcx = open("a_m.pcx", shipped), colour = open("a_c.tga", tga24), blend = open("a_d1.tga", tga32);
	TEST_EXPECT(pcx && colour && blend && pcx->paintable() && colour->paintable() && blend->paintable());
	TEST_EXPECT(pcx->plane(0).format() == RasterFormat::U8 && colour->plane(0).format() == RasterFormat::Rgb8 &&
	            blend->plane(0).format() == RasterFormat::Rgba8);
	// The plane is the picture the game draws: the top row first, red first.
	TEST_EXPECT(colour->plane(0).texel(1, 0)[0] == 5 && colour->plane(0).texel(1, 0)[2] == 254);
	TEST_EXPECT(bytes_of(*pcx) == shipped && bytes_of(*colour) == tga24);
	// A PNG of colours: a plane of RGBA; a DDS and a run-length TGA held whole.
	const std::vector<uint8_t> png = png::encode_png_rgba(rgba(4, 4).data(), 4, 4);
	const auto source = open("art/src.png", png);
	TEST_EXPECT(source && source->paintable() && source->plane(0).format() == RasterFormat::Rgba8);
	// A palette PNG (a terrain set's surface or foliage map: its indices the classes or codes) and a grey one (a
	// foliage map's levels): a U8 plane each, written again in their form.
	const IndexedImage8 classes = indexed(6, 4);
	std::vector<uint8_t> entries;
	for (int i = 0; i < 20; ++i) entries.insert(entries.end(), {classes.palette[i][0], classes.palette[i][1], classes.palette[i][2]});
	std::vector<uint8_t> small = classes.indices;
	for (uint8_t &index : small) index = uint8_t(index % 20);
	const std::vector<uint8_t> palette_png = png::encode_png_indexed(small.data(), 6, 4, entries.data(), 20);
	const std::vector<uint8_t> grey_png = png::encode_png_grey(small.data(), 6, 4);
	const auto surface = open("art/isle_surface.png", palette_png), levels = open("art/isle_foliage.png", grey_png);
	TEST_EXPECT(surface && surface->paintable() && surface->plane(0).format() == RasterFormat::U8 &&
	            surface->plane(0).u8(5, 3) == small[23]);
	TEST_EXPECT(levels && levels->paintable() && levels->plane(0).format() == RasterFormat::U8 && levels->plane(0).u8(1, 0) == small[1]);
	TEST_EXPECT(written_again("art/isle_surface.png", palette_png) && written_again("art/isle_foliage.png", grey_png));
	std::vector<uint8_t> dds_bytes;
	TEST_EXPECT(dds::dds_write_a8r8g8b8(rgba(4, 4).data(), 4, 4, dds_bytes, why));
	const auto dds = open("a.dds", dds_bytes);
	std::vector<uint8_t> rle = tga32;
	rle[2] = 10;
	const auto packed = open("b.tga", rle);
	TEST_EXPECT(dds && !dds->paintable() && dds->plane_count() == 0 && bytes_of(*dds) == dds_bytes);
	TEST_EXPECT(packed && !packed->paintable() && bytes_of(*packed) == rle);
	TEST_EXPECT(!texture_unpaintable_words("a.dds", dds->state()).empty());
	std::printf("forms: PCX (both coders), TGA (24 and 32, the footer), PNG (colour, palette, grey) painted; DDS and RLE TGA whole\n");
	return 0;
}

int test_paint_undo_save() {
	std::string why;
	std::vector<uint8_t> file;
	TEST_EXPECT(encode_pcx_indexed(indexed(128, 64), file, why));
	auto document = open("t_m.pcx", file);
	TEST_EXPECT(document && !document->dirty());
	const uint64_t generation = document->load_generation(), revision = document->revision();
	Diagnostic error;
	// One texel painted: one step, the texel's tile the region, the image the game reads changed.
	const uint8_t was = document->plane(0).u8(100, 50);
	TEST_EXPECT(document->apply({paint(*document, 100, 50, {uint8_t(was + 1)})}, error));
	TEST_EXPECT(document->dirty() && document->plane(0).u8(100, 50) == uint8_t(was + 1) && bytes_of(*document) != file);
	ChangeSet changes;
	TEST_EXPECT(document->changes_since(generation, revision, changes));
	const RasterChanges *raster = std::get_if<RasterChanges>(&changes);
	TEST_EXPECT(raster && raster->regions.size() == 1 && raster->regions[0].x == 64 && raster->regions[0].y == 0 &&
	            raster->regions[0].width == 64 && raster->regions[0].height == 64);
	TEST_EXPECT(document->image() && document->image()->width() == 128);
	// What a Save writes reads back as the painted texels in the file's form.
	const std::vector<uint8_t> painted = bytes_of(*document);
	IndexedImage8 read;
	PcxIndexedForm form;
	TEST_EXPECT(decode_pcx_indexed(painted.data(), painted.size(), read, why, &form) &&
	            read.indices[size_t(50) * 128 + 100] == uint8_t(was + 1) && form.coding == PcxCoding::Shipped);
	// Undone: the file as read, the document clean; done again.
	document->undo();
	TEST_EXPECT(!document->dirty() && bytes_of(*document) == file);
	document->redo();
	TEST_EXPECT(document->dirty() && bytes_of(*document) == painted);
	document->undo();
	// A stroke's batches under one gesture: one step.
	const uint64_t gesture = next_edit_gesture();
	TEST_EXPECT(document->apply({paint(*document, 1, 1, {200}, gesture)}, error) &&
	            document->apply({paint(*document, 2, 1, {200}, gesture)}, error));
	document->end_edit_group();
	document->undo();
	TEST_EXPECT(!document->dirty() && document->plane(0).u8(1, 1) != 200 && bytes_of(*document) == file);
	// A paint made for another plane (its size) is refused, the texture as it was.
	auto stale = std::make_shared<TexturePaintEdit>();
	stale->format = RasterFormat::U8;
	stale->width = 4;
	stale->height = 4;
	stale->at = {0};
	stale->values = {1};
	TEST_EXPECT(!document->apply({RasterDocument::edit(stale)}, error) && !document->dirty());
	// A texture operation is a whole change: the region answer cannot say across it; undone, the plane again.
	auto resized = std::make_shared<TextureImageEdit>();
	std::string words;
	TextureOperation operation;
	operation.kind = TextureOperationKind::Resize;
	operation.params = {{"size", "64x32"}};
	TEST_EXPECT(apply_texture_operation("t_m.pcx", file, operation, resized->bytes, words, why));
	Edit whole;
	whole.operation = EditOperation::Apply;
	whole.payload = resized;
	const uint64_t before = document->revision();
	TEST_EXPECT(document->apply({whole}, error) && document->plane(0).width() == 64 && document->paintable());
	TEST_EXPECT(!document->changes_since(document->load_generation(), before, changes));
	document->undo();
	TEST_EXPECT(document->plane(0).width() == 128 && bytes_of(*document) == file);
	// A brush on a texture held whole is refused.
	std::vector<uint8_t> dds_bytes;
	TEST_EXPECT(dds::dds_write_a8r8g8b8(rgba(4, 4).data(), 4, 4, dds_bytes, why));
	auto dds = open("a.dds", dds_bytes);
	auto onto = std::make_shared<TexturePaintEdit>();
	onto->format = RasterFormat::Rgba8;
	onto->width = 4;
	onto->height = 4;
	onto->at = {0};
	onto->values = {1, 2, 3, 4};
	TEST_EXPECT(dds && !dds->apply({RasterDocument::edit(onto)}, error) && !dds->dirty());
	std::printf("paint: one step a stroke, its tiles the region, undone to the file as read, a resize whole\n");
	return 0;
}

std::string lower(std::string s) {
	for (char &c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

bool ends_with(const std::string &s, const char *tail) {
	const std::string t(tail);
	return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
}

// Every shipped terrain map: written again from its plane as the same bytes; painted at one texel, undone, saved
// identical.
int test_retail_maps() {
	if (!retail::selected()) return 0;
	const std::string assets = retail::assets();
	if (assets.empty()) {
		retail::skip_leg("OPENNOVA_JO_ASSETS (the shipped _m/_f.pcx and _c/_d1.tga terrain maps)");
		return 0;
	}
	int swept = 0, failures = 0;
	for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(assets)) {
		if (!entry.is_regular_file()) continue;
		const std::string name = entry.path().filename().string(), l = lower(name);
		if (!ends_with(l, "_m.pcx") && !ends_with(l, "_f.pcx") && !ends_with(l, "_c.tga") && !ends_with(l, "_d1.tga")) continue;
		const std::vector<uint8_t> bytes = test_io::read_file(entry.path().string());
		auto document = open(name, bytes);
		const bool painted = document && document->paintable() && written_again(name, bytes);
		bool same = false;
		if (painted) {
			Diagnostic error;
			const RasterPlane &plane = document->plane(0);
			std::vector<uint8_t> value(plane.texel(0, 0), plane.texel(0, 0) + plane.texel_bytes());
			value[0] = uint8_t(value[0] ^ 0x55);
			same = document->apply({paint(*document, 0, 0, value)}, error) && bytes_of(*document) != bytes;
			document->undo();
			same = same && !document->dirty() && bytes_of(*document) == bytes;
		}
		++swept;
		if (!painted || !same) {
			std::printf("FAIL: %s (%s)\n", name.c_str(), !painted ? "its plane does not write it again" : "undone it differs");
			++failures;
		}
	}
	if (swept == 0) {
		std::printf("FAIL: no terrain map under %s\n", assets.c_str());
		return 1;
	}
	std::printf("retail: %d terrain maps painted, undone and written again, %d failures\n", swept, failures);
	return failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	const int failures = test_forms() + test_paint_undo_save() + test_retail_maps();
	if (failures == 0) std::printf("editor_texture_paint: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
