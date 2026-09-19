#include <runtime/renderer/material_texture.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

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
// [orig: load_texture_as_normalmap @0x58C985..0x58CAED (the live type-4/5
// kernel); Texture_ApplyNormalMapFilter @0x58BD90..0x58C06C (its uncalled
// instruction-for-instruction twin)]
int main() {
    expect(normal_material_filename("brick.TGA", false, true) == "brick.dds", "packed normals prefer the DDS sibling");
    expect(normal_material_filename("brick.TGA", true, true) == "brick.TGA", "loose TGA override wins");
    expect(normal_material_filename("brick.TGA", false, false) == "brick.TGA", "missing DDS uses the authored TGA");
    expect(normal_material_filename("brick.MDT", false, true) == "brick.MDT", "MDT has no extension fallback");
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
			"unsupported rows and failed loads bind the checkerboard");
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
