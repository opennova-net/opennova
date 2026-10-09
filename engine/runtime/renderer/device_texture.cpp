#include "device_texture.h"

#include <algorithm>

namespace opennova::renderer {

namespace {

// The levels' bytes of a chain of `levels` from `width` x `height`, each level half the one before (at
// least 1 a side).
uint64_t chain_bytes(uint32_t width, uint32_t height, uint32_t levels, DeviceTextureFormat format, uint32_t bits) {
	uint64_t bytes = 0;
	for (uint32_t level = 0; level < levels; ++level) {
		bytes += device_level_bytes(width, height, format, bits);
		width = std::max(1u, width >> 1);
		height = std::max(1u, height >> 1);
	}
	return bytes;
}

// The cap the creation flags set, else the card's side [orig: GTexture_DownsampleToLimits @ 0x68718E..0x6871AC:
// each flag sets the cap outright, the last one tested winning].
uint32_t flag_cap(uint32_t flags, uint32_t max_side) {
	uint32_t cap = max_side;
	if ((flags & kTextureFlagCap512) != 0) cap = 512;
	if ((flags & kTextureFlagCap256) != 0) cap = 256;
	if ((flags & kTextureFlagCap128) != 0) cap = 128;
	return cap;
}

} // namespace

// [orig: Render_SetObjectTexDetailFlags @ 0x5B1F40: level 0 stores 0x20000 in all three words
// (@ 0x5B1F4F..0x5B1F59); 1 stores 0x10000, 0x10000, 0x20000 (@ 0x5B1F69..0x5B1F73); 2 stores 0, 0x10000,
// 0x10000 (@ 0x5B1F81..0x5B1F93); any other 0 in all three (@ 0x5B1F81, @ 0x5B1F99..0x5B1F9F).
// Material_LoadStageTexture @ 0x5B16F4..0x5B1723: the row's slot byte (+0x10) less one through the table
// @ 0x5B181C (0, 1, 3, 3, 3, 3, 3, 2, 2): slot 1 the word at 0x28E0874, slot 2 the one at 0x28E0878, slots 8
// and 9 the one at 0x28E087C, slots 3 to 7 and any other none]
uint32_t object_texdetail_flags(int level, uint8_t slot) {
	uint32_t slot1 = 0, slot2 = 0, slot89 = 0;
	switch (level) {
	case 0: slot1 = slot2 = slot89 = kTextureFlagHalveTwice; break;
	case 1:
		slot1 = slot2 = kTextureFlagHalveOnce;
		slot89 = kTextureFlagHalveTwice;
		break;
	case 2: slot2 = slot89 = kTextureFlagHalveOnce; break;
	default: break;
	}
	switch (slot) {
	case 1: return slot1;
	case 2: return slot2;
	case 8:
	case 9: return slot89;
	default: return 0;
	}
}

// [orig: GTexture_DownsampleToLimits @ 0x687170 — the cap's halvings while either side exceeds it
// (@ 0x6871B7..0x6871BD); 0x20000 shifts both sides by 2 and counts two (@ 0x6871C8..0x6871D0), else 0x10000
// enters the card's loop at its halving (@ 0x6871DB, a halving whatever the size); the card's halvings while
// either side exceeds dword_32A623C (@ 0x6871EA)]
uint32_t pixel_texture_halvings(uint32_t width, uint32_t height, uint32_t flags, uint32_t max_side) {
	// The sides are signed ints there; a texture's are far below 2^31.
	const uint32_t cap = flag_cap(flags, max_side);
	uint32_t halvings = 0;
	while (width > cap || height > cap) {
		width >>= 1;
		height >>= 1;
		++halvings;
	}
	if ((flags & kTextureFlagHalveTwice) != 0) {
		width >>= 2;
		height >>= 2;
		halvings += 2;
	} else if ((flags & kTextureFlagHalveOnce) != 0) {
		width >>= 1;
		height >>= 1;
		++halvings;
	}
	while (width > max_side || height > max_side) {
		width >>= 1;
		height >>= 1;
		++halvings;
	}
	return halvings;
}

const char *device_texture_format_name(DeviceTextureFormat format) {
	switch (format) {
	case DeviceTextureFormat::A8R8G8B8: return "A8R8G8B8";
	case DeviceTextureFormat::Dxt1: return "DXT1";
	case DeviceTextureFormat::Dxt3: return "DXT3";
	case DeviceTextureFormat::Dxt5: return "DXT5";
	case DeviceTextureFormat::Uncompressed: break;
	}
	return "uncompressed";
}

uint64_t device_level_bytes(uint32_t width, uint32_t height, DeviceTextureFormat format, uint32_t bits) {
	const uint64_t blocks = uint64_t(std::max(1u, (width + 3) / 4)) * std::max(1u, (height + 3) / 4);
	switch (format) {
	case DeviceTextureFormat::Dxt1: return blocks * 8;
	case DeviceTextureFormat::Dxt3:
	case DeviceTextureFormat::Dxt5: return blocks * 16;
	case DeviceTextureFormat::A8R8G8B8: return uint64_t(width) * height * 4;
	case DeviceTextureFormat::Uncompressed: break;
	}
	return (uint64_t(width) * height * bits + 7) / 8;
}

uint32_t full_chain_levels(uint32_t width, uint32_t height) {
	uint32_t levels = 1;
	for (uint32_t side = std::max(width, height); side > 1; side >>= 1) ++levels;
	return levels;
}

// [orig: GTexture_StatBytesForFormat @ 0x686EF0: DXT1 (0x31545844) (w * h) >> 1 (@ 0x686F16); DXT2 to DXT5 w * h (@ 0x686EFB,
// @ 0x686F1C, @ 0x686F1E); any other w * h times its bytes a texel (@ 0x686F38)]
uint64_t texture_stat_bytes(uint32_t width, uint32_t height, DeviceTextureFormat format, uint32_t bits) {
	const uint64_t texels = uint64_t(width) * height;
	switch (format) {
	case DeviceTextureFormat::Dxt1: return texels >> 1;
	case DeviceTextureFormat::Dxt3:
	case DeviceTextureFormat::Dxt5: return texels;
	case DeviceTextureFormat::A8R8G8B8: return texels * 4;
	case DeviceTextureFormat::Uncompressed: break;
	}
	return texels * (bits / 8);
}

// [orig: GTexture_CreateFromPixelData_0 @ 0x6876C0 — the halvings @ 0x687785 (GTexture_DownsampleToLimits),
// the stat @ 0x6877A3..0x6877AB over the halved sides, the levels @ 0x6877BA..0x687801 (texture_level_count),
// D3DXCreateTexture over the halved sides in the A8R8G8B8 the pixels are (@ 0x68781A, D3DPOOL_MANAGED)]
DeviceTexture pixel_device_texture(uint32_t width, uint32_t height, uint32_t flags, uint32_t max_side) {
	DeviceTexture out;
	out.format = DeviceTextureFormat::A8R8G8B8;
	out.halvings = pixel_texture_halvings(width, height, flags, max_side);
	const uint32_t shift = std::min(out.halvings, 31u);
	out.width = std::max(1u, width >> shift);
	out.height = std::max(1u, height >> shift);
	out.levels = texture_level_count(out.width, out.height, flags);
	out.bytes = chain_bytes(out.width, out.height, out.levels, out.format, out.bits);
	out.stat_bytes = texture_stat_bytes(out.width, out.height, out.format, out.bits);
	return out;
}

// [orig: GTexture_CreateFromPixelData_0 @ 0x6876C0]
std::vector<DeviceTextureLevel> pixel_device_texture_levels(const uint8_t *rgba, uint32_t width,
		uint32_t height, uint32_t flags, const TextureDxtCaps &caps, uint32_t level_limit,
		uint32_t max_side) {
	std::vector<DeviceTextureLevel> levels;
	if (rgba == nullptr || width == 0 || height == 0)
		return levels;
	std::vector<uint8_t> base(rgba, rgba + static_cast<size_t>(width) * height * 4);
	halve_rgba_times(base, width, height, pixel_texture_halvings(width, height, flags, max_side));
	uint32_t count = texture_level_count(width, height, flags);
	if (level_limit != 0)
		count = std::min(count, level_limit);
	const TextureDxtFormat format = select_texture_dxt_format(flags, caps);
	if (format != TextureDxtFormat::None) {
		for (const DxtSurface &surface : build_dxt_texture_levels(base.data(), width, height, format, count)) {
			DeviceTextureLevel level;
			level.width = surface.width;
			level.height = surface.height;
			level.rgba = encode_rgba8(decode_dxt_surface(surface));
			levels.push_back(std::move(level));
		}
		return levels;
	}
	DeviceTextureLevel level0;
	level0.width = width;
	level0.height = height;
	level0.rgba = std::move(base);
	levels.push_back(std::move(level0));
	extend_box_chain(levels, count);
	return levels;
}

// [orig: GTexture_InitFromMemory @ 0x687DF0 — the detail's skip @ 0x687E3B..0x687E49; the DXT5 made DXT1
// @ 0x687E90..0x687EB9 (sub_67C7F0, the card's DXT1, and dword_32656A0, the kept DXT5); the formats the card
// takes @ 0x687F5D, else A8R8G8B8 @ 0x688069; the skip grown by the cap or the card @ 0x6881AC..0x6881FC
// (each flag lowering the card's side only); a chain of no more levels than the skip and one loaded whole
// @ 0x68820A, @ 0x6882DE (MipLevels D3DX_DEFAULT); with no skip the file through D3DX with MipLevels 0
// @ 0x688315, D3DX's complete chain; the stat over the sides shifted by the skip @ 0x688328..0x688398]
DeviceTexture dds_device_texture(const DdsSource &source, uint32_t flags, const TextureDxtCaps &caps, uint32_t max_side) {
	DeviceTexture out;
	out.format = source.format;
	out.bits = source.bits;
	if (source.format == DeviceTextureFormat::Dxt5 && source.dxt5_opaque && caps.dxt1 && !caps.keep_dxt5) {
		out.format = DeviceTextureFormat::Dxt1;
		out.dxt5_as_dxt1 = true;
	}
	const bool takes = (out.format != DeviceTextureFormat::Dxt1 || caps.dxt1) &&
	                   ((out.format != DeviceTextureFormat::Dxt3 && out.format != DeviceTextureFormat::Dxt5) || caps.dxt5);
	uint32_t skip = (flags & kTextureFlagHalveTwice) != 0 ? 2u : (flags & kTextureFlagHalveOnce) != 0 ? 1u : 0u;
	// The cap and the card grow the skip, where it is taken at all: a format the card takes skips only for
	// the detail (@ 0x688110), a format it does not is decoded level by level from the skip (@ 0x687FEA).
	const auto grow = [&]() {
		uint32_t cap = max_side;
		if ((flags & kTextureFlagCap512) != 0 && cap > 512) cap = 512;
		if ((flags & kTextureFlagCap256) != 0 && cap > 256) cap = 256;
		if ((flags & kTextureFlagCap128) != 0 && cap > 128) cap = 128;
		uint32_t w = source.width >> std::min(skip, 31u), h = source.height >> std::min(skip, 31u);
		while (w > cap || h > cap) {
			w >>= 1;
			h >>= 1;
			++skip;
		}
	};
	const uint32_t full = full_chain_levels(std::max(1u, source.width), std::max(1u, source.height));
	if (!takes) {
		// D3DX loads the file in its own format with its complete chain, then each level from the skip is
		// copied into an A8R8G8B8 texture [orig: @ 0x687F9F, @ 0x688069..0x6880E0].
		grow();
		out.format = DeviceTextureFormat::A8R8G8B8;
		out.bits = 32;
		out.halvings = std::min(skip, full - 1);
		out.width = std::max(1u, source.width >> out.halvings);
		out.height = std::max(1u, source.height >> out.halvings);
		out.levels = full - out.halvings;
	} else if (skip == 0) {
		out.width = std::max(1u, source.width);
		out.height = std::max(1u, source.height);
		out.levels = full;
	} else {
		grow();
		const uint32_t file_levels = std::max(1u, source.levels);
		if (file_levels <= skip + 1) {
			out.whole = true;
			out.width = std::max(1u, source.width);
			out.height = std::max(1u, source.height);
			out.levels = full;
		} else {
			out.halvings = skip;
			out.width = std::max(1u, source.width >> skip);
			out.height = std::max(1u, source.height >> skip);
			out.levels = file_levels - skip;
		}
	}
	out.bytes = chain_bytes(out.width, out.height, out.levels, out.format, out.bits);
	// The game counts the sides shifted by the skip, the whole load's too (@ 0x688328..0x68832F).
	const uint32_t shift = std::min(skip, 31u);
	out.stat_bytes = texture_stat_bytes(std::max(1u, source.width >> shift), std::max(1u, source.height >> shift), out.format, out.bits);
	return out;
}

DeviceTextureFormat dds_device_format(std::string_view format_name) {
	if (format_name == "DXT1") return DeviceTextureFormat::Dxt1;
	if (format_name == "DXT2" || format_name == "DXT3") return DeviceTextureFormat::Dxt3;
	if (format_name == "DXT4" || format_name == "DXT5") return DeviceTextureFormat::Dxt5;
	if (format_name == "A8R8G8B8" || format_name.empty()) return DeviceTextureFormat::A8R8G8B8;
	return DeviceTextureFormat::Uncompressed;
}

// [orig: GTexture_InitFromMemory @ 0x68830E..0x688310 (D3DX_DEFAULT sides);
// D3DXCreateTextureFromFileInMemoryEx_Internal @ 0x6914D9..0x6914EE, @ 0x69150B..0x691520 (each
// rounded up to a power of two); CBlt::BltNone @ 0x6E0D57]
uint32_t d3dx_default_texture_side(uint32_t side) {
	uint32_t out = 1;
	while (out < side && out < 0x80000000u) out <<= 1;
	return out;
}

// [orig: Material_LoadStageTexture @ 0x5B173E..0x5B1742 (stage), @ 0x5B174F..0x5B1758 (plain),
// @ 0x5B1782..0x5B1790 (normal: or eax, 1000h)]
uint32_t model_row_texture_flags(TextureLoader loader, uint8_t slot, int level) {
	uint32_t flags = object_texdetail_flags(level, slot);
	if (loader == TextureLoader::Normal) flags |= kTextureFlagCap512;
	return flags;
}

// [orig: Texture_LoadByNameWithChannel @ 0x58B616 (a .dds through GTexture_InitFromMemory), @ 0x58B74D
// (pixels); Texture_LoadAndRegister @ 0x58B920; Texture_LoadAsNormalMap @ 0x58C6CB]
DeviceTexture model_row_device_texture(TextureLoader loader, uint8_t slot, int level, const DdsSource &file,
		bool dds) {
	const uint32_t flags = model_row_texture_flags(loader, slot, level);
	if (dds && loader == TextureLoader::Stage) return dds_device_texture(file, flags);
	return pixel_device_texture(file.width, file.height, flags);
}

// [orig: convert_dxt_alpha_blocks @ 0x687AC0 — the blocks counted @ 0x687B28..0x687B44 (each side over 4, at
// least 1); the first word of each block from the level's start, 16 bytes apart, while it is 0xFFFF
// (@ 0x687B56..0x687B6A)]
bool dxt5_first_level_opaque(const uint8_t *blocks, size_t size, uint32_t width, uint32_t height) {
	const uint64_t count = uint64_t(std::max(1u, height / 4)) * std::max(1u, width / 4);
	if (blocks == nullptr || count * 16 > size) return false;
	for (uint64_t block = 0; block < count; ++block)
		if (blocks[block * 16] != 0xFF || blocks[block * 16 + 1] != 0xFF) return false;
	return true;
}

} // namespace opennova::renderer
