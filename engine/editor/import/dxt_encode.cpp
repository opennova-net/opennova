#include <editor/import/dxt_encode.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <thread>

#include <rgbcx.h> // third_party/rgbcx: the BC1/BC3 block encoder
#include <runtime/renderer/texture_dxt.h>

namespace opennova::editor {

namespace {

// rgbcx's recommended level: past it, 5 to 9 times the time buys 0.1 to 0.2 dB on the base game's textures.
constexpr uint32_t kRgbcxLevel = 10;

void init_rgbcx() {
	// Its BC1 tables, global state built once before any thread encodes.
	static std::once_flag once;
	std::call_once(once, [] { rgbcx::init(rgbcx::bc1_approx_mode::cBC1Ideal); });
}

// One level's blocks: its texels at full precision (`colours`) and as bytes (`bytes`, what rgbcx reads).
// A block past the right or bottom edge repeats the edge texels as the D3DX codec does
// [orig: D3DXTex::CCodecDXT::Encode @ 0x6EDE25..0x6EDEC6, renderer::encode_dxt_surface].
void encode_level(const std::vector<renderer::DxtColor> &colours, const std::vector<uint8_t> &bytes, uint32_t width,
                  uint32_t height, bool dxt5, std::vector<uint8_t> &out) {
	// Past an edge, texel column (row) 1 and 2 repeat texel 0 and column 3 repeats column 1, itself
	// repeated from 0 where the block holds one column alone.
	static constexpr uint32_t kEdgeSource[4] = {0, 0, 0, 1};
	const auto inside = [](uint32_t at, uint32_t valid) {
		while (at >= valid) at = kEdgeSource[at];
		return at;
	};
	const uint32_t blocks_x = (width + 3) / 4, blocks_y = (height + 3) / 4;
	const size_t block_bytes = dxt5 ? 16 : 8;
	out.assign(size_t(blocks_x) * blocks_y * block_bytes, 0);
	const auto rows = [&](uint32_t first, uint32_t last) {
		for (uint32_t by = first; by < last; ++by)
			for (uint32_t bx = 0; bx < blocks_x; ++bx) {
				const uint32_t valid_w = std::min(4u, width - bx * 4), valid_h = std::min(4u, height - by * 4);
				uint8_t texels[64];
				renderer::DxtBlockColors block{};
				bool keyed = false;
				for (uint32_t y = 0; y < 4; ++y)
					for (uint32_t x = 0; x < 4; ++x) {
						const uint32_t sx = inside(x, valid_w), sy = inside(y, valid_h);
						const size_t at = size_t(by * 4 + sy) * width + bx * 4 + sx;
						std::memcpy(&texels[(y * 4 + x) * 4], &bytes[at * 4], 4);
						block[y * 4 + x] = colours[at];
						keyed = keyed || texels[(y * 4 + x) * 4 + 3] < 128;
					}
				uint8_t *dst = out.data() + (size_t(by) * blocks_x + bx) * block_bytes;
				if (dxt5)
					rgbcx::encode_bc3(kRgbcxLevel, dst, texels);
				else if (keyed)
					renderer::encode_dxt1_block(block, false, dst);
				else
					rgbcx::encode_bc1(kRgbcxLevel, dst, texels, true, false);
			}
	};
	// Rows of blocks shared out over the machine's threads: a 4096 x 4096 level is a million blocks.
	const uint32_t threads = std::min<uint32_t>(std::max(1u, std::thread::hardware_concurrency()), std::max(1u, blocks_y / 8));
	if (threads <= 1) {
		rows(0, blocks_y);
		return;
	}
	std::vector<std::thread> pool;
	const uint32_t per = (blocks_y + threads - 1) / threads;
	for (uint32_t first = 0; first < blocks_y; first += per) pool.emplace_back(rows, first, std::min(blocks_y, first + per));
	for (std::thread &each : pool) each.join();
}

} // namespace

uint32_t dxt_full_chain_levels(uint32_t width, uint32_t height) {
	uint32_t levels = 1;
	for (uint32_t side = std::max(width, height); side > 1; side >>= 1) ++levels;
	return levels;
}

std::vector<std::vector<uint8_t>> encode_dxt_levels(const uint8_t *rgba, uint32_t width, uint32_t height, bool dxt5,
                                                    bool full_chain) {
	std::vector<std::vector<uint8_t>> levels;
	if (rgba == nullptr || width == 0 || height == 0) return levels;
	init_rgbcx();
	const uint32_t count = full_chain ? dxt_full_chain_levels(width, height) : 1;
	std::vector<renderer::DxtColor> colours = renderer::decode_rgba8(rgba, width, height);
	std::vector<uint8_t> bytes(rgba, rgba + size_t(width) * height * 4);
	for (uint32_t level = 0; level < count; ++level) {
		if (level > 0) {
			colours = renderer::box_filter_half(colours, width, height);
			width = std::max(1u, width / 2);
			height = std::max(1u, height / 2);
			bytes = renderer::encode_rgba8(colours);
		}
		levels.emplace_back();
		encode_level(colours, bytes, width, height, dxt5, levels.back());
	}
	return levels;
}

} // namespace opennova::editor
