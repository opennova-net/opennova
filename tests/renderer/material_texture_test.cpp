#include <runtime/renderer/material_texture.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <base/io/os_path.h>

using namespace opennova::renderer;

void expect(bool condition, const char *message) {
	if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
std::vector<uint8_t> rgba_from_bgra_hex(const char *text) {
	std::vector<uint8_t> out;
	for (size_t i = 0; text[i]; i += 2)
		out.push_back(static_cast<uint8_t>(std::stoul(std::string(text + i, 2), nullptr, 16)));
	for (size_t i = 0; i < out.size(); i += 4) std::swap(out[i], out[i + 2]);
	return out;
}

// Synthetic images executed by the retail x86 kernel (all pixel math intact).
// Kernel SHA256 0be6f2d7022549d57ccb3955007c20bef9b31f35aa7accf3886551be24e702a6.
// [orig: Texture_LoadAsNormalMap @0x58C985..0x58CAED (the live type-4/5
// kernel); Texture_ApplyNormalMapFilter @0x58BD90..0x58C06C (its uncalled
// instruction-for-instruction twin)]
int main() {
	{
		const auto source = rgba_from_bgra_hex("113377bb725888f0d37d992534a2aa5a95c7bb8ff6ecccc45711ddf9b836ee2e195bff637a801098dba521cd3cca32029def4337fe14546c5f3965a1c05e76d6");
		const auto expected = rgba_from_bgra_hex("f28648bbc58315f0f286b625c583e95af278488ff278b6c4f278b6f9f278482ef2784863c57b1598f278b6cdc57be902f2864837f286b66cf286b6a1f28648d6");
		expect(normal_map_from_height_rgba(source.data(), 4, 4, 0.015625f) == expected, "retail normal-map pixel witness");
		// TGA swaps input A/B before the same height kernel. Keep source
		// blue independent from height and preserve it as output alpha.
		auto tga = source;
		for (size_t i = 0; i < tga.size(); i += 4) std::swap(tga[i + 2], tga[i + 3]);
		expect(normal_map_from_height_rgba(tga.data(), 4, 4, 0.015625f, 3, 2) == expected, "TGA alpha height and blue alpha lanes");
	}
	{
		const auto source = rgba_from_bgra_hex("113377bb725888f0d37d992534a2aa5a95c7bb8ff6ecccc45711ddf9b836ee2e195bff637a801098dba521cd3cca32029def4337fe14546c5f3965a1c05e76d62183870b82a89840e3cda97544f2baaaa517cbdf063cdc146761ed49c886fe7e29ab0fb38ad020e8ebf5311d4c1a4252ad3f53870e6464bc6f8975f1d0ae8626");
		const auto expected = rgba_from_bgra_hex("ee8dbcbbc58815f0f18db625f18db65ac588158fb9119bc4f18db6f9ee8dbc2eee71bc63c5761598f171b6cdf171b602c5761537b9ed9b6cf171b6a1ee71bcd6ee71bc0bc5761540f171b675f171b6aaf171b6dfb9ed9b14c5761549ee71bc7eee8dbcb3c58815e8f18db61df18db652f18db687b9119bbcc58815f1ee8dbc26");
		expect(normal_map_from_height_rgba(source.data(), 8, 4, 0.015625f) == expected, "retail normal-map pixel witness");
		// TGA swaps input A/B before the same height kernel. Keep source
		// blue independent from height and preserve it as output alpha.
		auto tga = source;
		for (size_t i = 0; i < tga.size(); i += 4) std::swap(tga[i + 2], tga[i + 3]);
		expect(normal_map_from_height_rgba(tga.data(), 8, 4, 0.015625f, 3, 2) == expected, "TGA alpha height and blue alpha lanes");
	}
	{
		const auto source = rgba_from_bgra_hex("113377bb725888f0d37d992534a2aa5a95c7bb8ff6ecccc45711ddf9b836ee2e195bff63");
		const auto expected = rgba_from_bgra_hex("c2a4e5bbc2a419f0b62f2c25c25ae55ac25a198fb6cf2cc4e6474df9e647b12ec5e4a163");
		expect(normal_map_from_height_rgba(source.data(), 3, 3, 0.015625f) == expected, "retail normal-map pixel witness");
		// TGA swaps input A/B before the same height kernel. Keep source
		// blue independent from height and preserve it as output alpha.
		auto tga = source;
		for (size_t i = 0; i < tga.size(); i += 4) std::swap(tga[i + 2], tga[i + 3]);
		expect(normal_map_from_height_rgba(tga.data(), 3, 3, 0.015625f, 3, 2) == expected, "TGA alpha height and blue alpha lanes");
	}
	{
		const auto source = rgba_from_bgra_hex("113377bb725888f0d37d992534a2aa5a95c7bb8ff6ecccc45711ddf9b836ee2e195bff637a801098dba521cd3cca32029def4337fe14546c5f3965a1c05e76d6");
		const auto expected = rgba_from_bgra_hex("a68f07bb8c8400f0a68ff7258c84fe5aa66f078fa66ff7c4a66ff7f9a66f072ea66f07638c7a0098a66ff7cd8c7afe02a68f0737a68ff76ca68ff7a1a68f07d6");
		expect(normal_map_from_height_rgba(source.data(), 4, 4, 0.1f) == expected, "retail normal-map pixel witness");
		// TGA swaps input A/B before the same height kernel. Keep source
		// blue independent from height and preserve it as output alpha.
		auto tga = source;
		for (size_t i = 0; i < tga.size(); i += 4) std::swap(tga[i + 2], tga[i + 3]);
		expect(normal_map_from_height_rgba(tga.data(), 4, 4, 0.1f, 3, 2) == expected, "TGA alpha height and blue alpha lanes");
	}
	{
		const auto source = rgba_from_bgra_hex("113377bb");
		const auto expected = rgba_from_bgra_hex("ff7f7fbb");
		expect(normal_map_from_height_rgba(source.data(), 1, 1, 0.015625f) == expected, "retail normal-map pixel witness");
		// TGA swaps input A/B before the same height kernel. Keep source
		// blue independent from height and preserve it as output alpha.
		auto tga = source;
		for (size_t i = 0; i < tga.size(); i += 4) std::swap(tga[i + 2], tga[i + 3]);
		expect(normal_map_from_height_rgba(tga.data(), 1, 1, 0.015625f, 3, 2) == expected, "TGA alpha height and blue alpha lanes");
	}
	expect(material_texture_transform(4, "Body.MDT", true) == MaterialTextureTransform::Unchanged,
			"MDT normals are already converted");
	expect(material_texture_transform(5, "Body.tga", true) == MaterialTextureTransform::NormalFromAlpha,
			"TGA normal rows convert alpha height");
	expect(material_texture_transform(0, "Body.tga", true) == MaterialTextureTransform::Unchanged,
			"diffuse TGA retains authored pixels");
	expect(material_texture_transform(3, "Body.tga", true) == MaterialTextureTransform::Checkerboard &&
			material_texture_transform(0, "Missing.tga", false) == MaterialTextureTransform::Checkerboard,
			"unsupported runtime rows and failed loads bind the checkerboard");
	// Pixel-built textures (TGA/MDT/PCX rows, normal maps, the checkerboard)
	// get one level per halving while the smaller side exceeds 2.
	// [orig: GTexture_CreateFromPixelData_0 @0x6877BC..0x6877D8]
	expect(pixel_texture_mip_levels(256, 256) == 7 && pixel_texture_mip_levels(128, 128) == 6 &&
			pixel_texture_mip_levels(256, 64) == 5 && pixel_texture_mip_levels(4, 4) == 1 &&
			pixel_texture_mip_levels(3, 8) == 1 && pixel_texture_mip_levels(2, 2) == 0 &&
			pixel_texture_mip_levels(48, 48) == 5,
			"pixel-built mip chains end at the last level above min-dim 2");
	// The name cut and the DDS sibling the loaders build.
	// [orig: Texture_LoadByNameWithChannel @0x58B4E1..0x58B598]
	expect(material_texture_query("Jbark_2.dds.tga") == "Jbark_2.dds" &&
			material_texture_query("wall.tga") == "wall.tga" &&
			material_texture_query("noext") == "noext",
			"the query keeps three characters after the first dot");
	expect(material_dds_sibling("wall.tga") == "wall.dds" &&
			material_dds_sibling("Jbark_2.dds") == "Jbark_2.dds" &&
			material_dds_sibling("noext") == "noext.dds",
			"the DDS sibling replaces the last extension");
	// The one file a row's loader opens and the reader that decodes it, by the
	// runtime type, the name, the files there and the loose-first hits.
	// [orig: Material_LoadStageTexture @0x5B16F0; Texture_LoadByNameWithChannel
	// @0x58B4E1..0x58B6E6; Texture_LoadAndRegister @0x58B80E..0x58B881;
	// Texture_LoadAsNormalMap @0x58C480; sub_58A430 @0x58A430]
	{
		using Reader = MaterialTextureReader;
		struct SourceCase {
			uint8_t type;
			const char *name;
			std::vector<std::string> files, loose;
			const char *file;
			Reader reader;
			const char *why;
		};
		const SourceCase cases[] = {
			{0, "wall.tga", {"wall.dds"}, {}, "wall.dds", Reader::Dds, "a diffuse row's DDS sibling wins"},
			{0, "wall.tga", {"wall.dds", "wall.tga"}, {"wall.tga"}, "wall.tga", Reader::Tga,
					"a loose-first hit on the query takes the plain path"},
			{0, "wall.tga", {}, {}, "wall.tga", Reader::Tga, "no sibling: the query through the TGA reader"},
			{2, "Jbark_2.dds.tga", {"Jbark_2.dds"}, {}, "Jbark_2.dds", Reader::Dds,
					"the query, cut after the first dot, is its own sibling"},
			{8, "Body.MDT", {"Body.dds"}, {}, "Body.MDT", Reader::Tga, "an upper-case .MDT query skips the DDS probe"},
			{0, "body.mdt", {"body.dds"}, {}, "body.dds", Reader::Dds, "the diffuse .MDT test is case-sensitive"},
			{0, "body.mdt", {}, {}, "body.mdt", Reader::Tga, "the plain path reads an .mdt through the TGA reader"},
			{0, "flag.pcx", {}, {}, "flag.pcx", Reader::Pcx, "a .pcx through the PCX reader"},
			{0, "photo.png", {}, {}, "", Reader::None, "any other name fails"},
			{0, "a.tga.pcx", {}, {}, "a.tga", Reader::Tga, "a diffuse row reads a.tga.pcx as its query a.tga"},
			{1, "wall.tga", {"wall.dds"}, {}, "wall.tga", Reader::Tga, "type 1 never probes a DDS sibling"},
			{1, "a.tga.pcx", {}, {}, "a.tga.pcx", Reader::Tga, "the plain path tests .TGA before .PCX"},
			{1, "flag.pcx", {"flag.dds"}, {}, "flag.pcx", Reader::Pcx, "type 1 reads a .pcx as a PCX"},
			{4, "brick.TGA", {"brick.dds"}, {}, "brick.dds", Reader::Dds, "a normal map's .TGA takes its DDS sibling"},
			{5, "brick.TGA", {"brick.dds", "brick.TGA"}, {"brick.TGA"}, "brick.TGA", Reader::Tga,
					"a loose-first hit on the name keeps the .TGA"},
			{4, "brick.TGA", {}, {}, "brick.TGA", Reader::Tga, "no sibling: the .TGA through the TGA reader"},
			{4, "brick.mdt", {"brick.dds"}, {}, "brick.mdt", Reader::Tga, "a normal .MDT, any case, skips the DDS probe"},
			{5, "a.mdt.tga", {"a.mdt.dds"}, {}, "a.mdt.tga", Reader::Tga, "the normal maps test .MDT before .TGA"},
			{5, "a.tga.pcx", {"a.tga.dds"}, {}, "a.tga.dds", Reader::Dds, "the sibling cuts the name at its last dot"},
			{4, "a.tga.pcx", {}, {}, "a.tga.pcx", Reader::Tga, "a .TGA name is read by the TGA reader, never as a PCX"},
			{4, "bump.pcx", {"bump.dds"}, {}, "", Reader::None, "a normal map's PCX test reads the empty second path"},
			{5, "bump.dds", {"bump.dds"}, {}, "", Reader::None, "a normal map named .dds loads nothing"},
			{6, "height.tga", {"height.dds"}, {}, "height.dds", Reader::Dds, "the horizon volume takes the sibling"},
			{7, "height.tga", {"height.dds", "height.tga"}, {"height.tga"}, "height.tga", Reader::Tga,
					"the occlusion map keeps a loose-first .TGA"},
			{7, "height.tga", {}, {}, "height.tga", Reader::Tga, "no sibling: the .TGA"},
			{6, "height.mdt", {"height.dds"}, {}, "", Reader::None, "the height producers read no .MDT"},
			{7, "height.pcx", {}, {}, "", Reader::None, "nor a .PCX"},
			{16, "field.nq8", {"field.dds"}, {}, "field.nq8", Reader::Chunk, "a chunk row reads the name as written"},
			{17, "hrz.tga", {"hrz.dds"}, {}, "hrz.tga", Reader::Chunk, "whatever it is called"},
			{18, "ao.bin", {}, {}, "ao.bin", Reader::Chunk, "and whether or not it is there"},
			{3, "wall.tga", {"wall.dds", "wall.tga"}, {}, "", Reader::None, "a type the dispatcher has no case for fails"},
			{12, "wall.tga", {"wall.tga"}, {}, "", Reader::None, "(the loader never stores one)"},
		};
		for (const SourceCase &c : cases) {
			const auto in = [](const std::vector<std::string> &names) {
				return [names](const std::string &file) {
					return std::find(names.begin(), names.end(), file) != names.end();
				};
			};
			const MaterialTextureSource source = material_texture_source(c.name, c.type, in(c.files), in(c.loose));
			if (source.file != c.file || source.reader != c.reader)
				std::fprintf(stderr, "type %u %s -> %s\n", unsigned(c.type), c.name, source.file.c_str());
			expect(source.file == c.file && source.reader == c.reader, c.why);
		}
		expect(material_texture_source("wall.tga", 0, {}).file == "wall.tga" &&
				material_texture_source("brick.tga", 4, {}, {}).reader == Reader::Tga,
				"an empty test answers false");
	}
	// What the DDS reader decodes a file as: D3DX's loader takes the first
	// format of its order whose test the bytes pass, of those the port decodes.
	// [orig: D3DXTex::CImage::Load @0x6DF1DC, the order @0x6DF212..0x6DF242]
	{
		using Format = DdsReaderFormat;
		const auto format = [](std::vector<uint8_t> bytes) { return dds_reader_format(bytes.data(), bytes.size()); };
		const auto tga = [](uint8_t map_type, uint8_t image_type, uint8_t depth, uint16_t side) {
			std::vector<uint8_t> bytes(18 + 4 * 4 * 4, 0);
			bytes[1] = map_type;
			bytes[2] = image_type;
			bytes[12] = static_cast<uint8_t>(side);
			bytes[14] = static_cast<uint8_t>(side);
			bytes[16] = depth;
			return bytes;
		};
		std::vector<uint8_t> dds(4 + 124, 0);
		dds[0] = 'D', dds[1] = 'D', dds[2] = 'S', dds[3] = ' ';
		expect(format(dds) == Format::Dds, "\"DDS \" and its 124-byte header");
		expect(format(std::vector<uint8_t>(dds.begin(), dds.begin() + 20)) == Format::None, "a DDS cut short");
		expect(format(tga(0, 2, 32, 4)) == Format::Tga && format(tga(0, 3, 8, 4)) == Format::Tga &&
				format(tga(0, 10, 24, 4)) == Format::Tga,
				"true-colour, grey and run-length TGA headers");
		expect(format(tga(2, 2, 32, 4)) == Format::None && format(tga(0, 0, 32, 4)) == Format::None &&
				format(tga(0, 2, 32, 0)) == Format::None && format(tga(0, 1, 8, 4)) == Format::None &&
				format(tga(0, 3, 32, 4)) == Format::None && format(tga(0, 2, 12, 4)) == Format::None,
				"D3DX's TGA test: the colour-map type, the image type, the sides, a mapped image's map, the depth");
		expect(format({0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0}) == Format::Png, "the PNG signature");
		expect(format({0xFF, 0xD8, 0xFF, 0xE0}) == Format::Jpeg, "the JPEG SOI marker");
		std::vector<uint8_t> bmp(14, 0);
		bmp[0] = 'B', bmp[1] = 'M', bmp[2] = 14;
		expect(format(bmp) == Format::Bmp, "\"BM\" with a file size that fits");
		bmp[2] = 15;
		expect(format(bmp) == Format::None, "a BMP's file size past the bytes");
		const std::string ppm = "P6\n4 4\n255\n";
		expect(format(std::vector<uint8_t>(ppm.begin(), ppm.end())) == Format::None &&
				dds_reader_format(nullptr, 0) == Format::None,
				"a PPM (decoded by D3DX, not the port) passes no later test");
	}
	// A row that loads only as a DDS sibling: a runtime 0/2/8 query no reader
	// of the plain path decodes.
	{
		std::string opens, loads;
		expect(material_texture_dds_only("photo.png", 0, opens, loads) && opens == "photo.png" && loads == "photo.dds",
				"a diffuse .png loads only as its .dds");
		expect(material_texture_dds_only("photo.png", 3, opens, loads),
				"an authored type the loader zeroes is a diffuse row");
		expect(!material_texture_dds_only("wall.tga", 0, opens, loads) &&
				!material_texture_dds_only("photo.png", 1, opens, loads) &&
				!material_texture_dds_only("photo.png", 4, opens, loads) &&
				!material_texture_dds_only("photo.png", 16, opens, loads) &&
				!material_texture_dds_only("photo.dds", 0, opens, loads) &&
				!material_texture_dds_only("", 0, opens, loads),
				"a readable name, another loader, a name that is its own sibling or no name");
	}
	// The loader never stores those runtime values: authored 3, 9..15 and
	// > 18 keep the memset zero and load as ordinary diffuse rows.
	// [orig: Material_ConvertDefinition @0x5B045B..0x5B04A0]
	for (unsigned authored = 0; authored < 256; ++authored) {
		const bool dropped = authored == 3 || (authored >= 9 && authored <= 15) || authored > 18;
		const uint8_t runtime = material_texture_runtime_type(static_cast<uint8_t>(authored));
		expect(runtime == (dropped ? 0 : authored), "loader texture-type remap");
	}
	expect(material_texture_transform(material_texture_runtime_type(3), "Body.tga", true) ==
					MaterialTextureTransform::Unchanged &&
			material_texture_transform(material_texture_runtime_type(12), "Body.tga", true) ==
					MaterialTextureTransform::Unchanged,
			"authored types the loader drops load as plain diffuse, not the checkerboard");
	// One result test for every row (test eax,eax @0x5B17F0 -> checkerboard
	// @0x5B17F4): a normal row whose source yields no readable image is a
	// failed load, never a null the material would replace with the flat normal.
	expect(material_texture_transform(5, "Body.tga", false) == MaterialTextureTransform::Checkerboard &&
			material_texture_transform(4, "Body.MDT", false) == MaterialTextureTransform::Checkerboard,
			"unreadable normal rows bind the checkerboard, not a null");
    const MaterialTextureTransform transforms[] = {MaterialTextureTransform::HorizonVolume,
        MaterialTextureTransform::AmbientOcclusion, MaterialTextureTransform::ChunkNormal,
        MaterialTextureTransform::ChunkHorizon, MaterialTextureTransform::ChunkOcclusion};
    const uint8_t types[] = {6, 7, 16, 17, 18};
    for (size_t i = 0; i < 5; ++i)
        expect(material_texture_transform(types[i], "Body.tga", true) == transforms[i], "dedicated producer dispatch");
    expect(material_texture_transform(6, "Body.png", true) == MaterialTextureTransform::Checkerboard,
        "the height producer requires TGA input");
    struct HorizonCase { uint32_t w, h; const char *source, *expected; };
    const HorizonCase horizon_cases[] = {
#include "material_horizon_vectors.inc"
    };
    const auto unhex = [](const char *hex) {
        std::vector<uint8_t> out;
        for (size_t i = 0; hex[i]; i += 2) out.push_back(uint8_t(std::stoul(std::string(hex+i,2),nullptr,16)));
        return out;
    };
    for (const auto &row : horizon_cases) {
        const auto source = unhex(row.source), expected = unhex(row.expected);
        const auto volume = horizon_volume_from_height(source.data(), row.w, row.h);
        expect(volume.width == row.w/4 && volume.height == row.h/4 && volume.depth == 16,
            "retail horizon volume dimensions");
        expect(volume.rgba == expected, "all horizon volume slices match the original instructions");
        const auto ao = ambient_occlusion_from_height(source.data(), row.w, row.h);
        expect(ao.rgba == std::vector<uint8_t>(row.w*row.h*4,255), "retail AO fills every output lane white");
    }
    const auto put = [](std::vector<uint8_t> &b, size_t at, uint32_t v) {
        for (int i=0;i<4;++i) b[at+i]=uint8_t(v>>(8*i));
    };
    for (uint8_t type : {16,17,18}) {
        const size_t count = type == 17 ? 8 : 4;
        std::vector<uint8_t> bytes(8+8+28+count*(type==16?4:1),0);
        const char *tag = type==16 ? "NQ8B" : type==17 ? "HRZ8" : "AOC8";
        for (int i=0;i<4;++i) bytes[8+i]=tag[i];
        put(bytes,12,uint32_t(bytes.size()-16));
        put(bytes,28,2); put(bytes,32,2); put(bytes,36,2);
        for (size_t i=44;i<bytes.size();++i) bytes[i]=uint8_t(i-44+1);
        const auto pixels = load_material_chunk(bytes.data(),bytes.size(),type);
        expect(pixels.width==2 && pixels.height==2 && pixels.depth==(type==17?2u:1u), "chunk dimensions");
        for (size_t i=0;i<count;++i) {
            expect(pixels.rgba[4*i+3]==(type==16?4*i+4:i+1), "chunk alpha lanes and slice order");
            expect(pixels.rgba[4*i]==(type==16?4*i+3:255) && pixels.rgba[4*i+2]==(type==16?4*i+1:255),
                "BGRA normals and A8 occlusion format mapping");
        }
        auto nested = bytes;
        nested.insert(nested.begin()+8,8,0); nested[8]='N'; nested[9]='E'; nested[10]='S'; nested[11]='T';
        put(nested,12,uint32_t(bytes.size()-8)|0x80000000u);
        expect(load_material_chunk(nested.data(),nested.size(),type).rgba==pixels.rgba,"nested producer chunk");
        expect(!load_material_chunk(bytes.data(),bytes.size()-1,type), "truncated chunk is a failed load");
        // A container of any of the three chunks, whatever its name; read whole or by its headers.
        expect(is_material_chunk_container(bytes) && is_material_chunk_container(nested), "a chunk container");
        const std::vector<uint8_t> truncated(bytes.begin(), bytes.end()-1);
        expect(!is_material_chunk_container(truncated), "a truncated chunk is no container");
        const std::filesystem::path file = std::filesystem::temp_directory_path() /
            ("opennova_material_chunk_" + std::to_string(type) + ".bin");
        { std::ofstream out(file, std::ios::binary); out.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size())); }
        uint64_t read = 0;
        expect(is_material_chunk_file(opennova::io::utf8_path(file), read) && read > 0 && read <= 3 * 8 + 28,
            "a chunk container file, read by its headers");
        std::filesystem::remove(file);
    }
    {
        // A large file of another kind: a megabyte of zeros reads as empty chunks, walked no further
        // than kChunkHeaderReads headers for each chunk type.
        const std::filesystem::path file = std::filesystem::temp_directory_path() / "opennova_material_chunk_filler.bin";
        { std::ofstream out(file, std::ios::binary); const std::vector<char> zeros(1 << 20, 0); out.write(zeros.data(), std::streamsize(zeros.size())); }
        uint64_t read = 0;
        expect(!is_material_chunk_file(opennova::io::utf8_path(file), read) && read > 0 &&
            read <= 3 * (kChunkHeaderReads * 8 + 28), "a file of no chunk costs a few small reads");
        std::filesystem::remove(file);
        read = 0;
        expect(!is_material_chunk_file(opennova::io::utf8_path(file), read) && read == 0, "no file, nothing read");
        expect(!is_material_chunk_container({}), "no bytes, no container");
    }
	const auto checker = missing_material_texture_rgba();
	expect(checker.size() == 128 * 128 * 4, "fallback dimensions");
	for (size_t i = 0; i < checker.size(); i += 4) {
		expect(checker[i] == checker[i + 1] && checker[i] == checker[i + 2] && checker[i + 3] == 255,
				"fallback is opaque gray");
	}
	expect(checker[0] == 0x30 && checker[3 * 4] == 0x30 && checker[4 * 4] == 0x50 &&
			checker[(4 * 128) * 4] == 0x50 && checker[(4 * 128 + 4) * 4] == 0x30 &&
			checker[(8 * 128 + 8) * 4] == 0x30, "four-pixel XOR checker squares");
	return 0;
}
