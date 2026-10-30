#include <runtime/renderer/triangle_strip.h>

#include <formats/env/env_water_render.h>

#include <cstdint>
#include <cstdio>
#include <iterator>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition)                                                        \
	do {                                                                        \
		if (!(condition)) {                                                      \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);     \
			++failures;                                                           \
		}                                                                       \
	} while (0)

using opennova::renderer::triangle_strip_to_list;

void check_parity_and_base() {
	// Triangle 0 as written, triangle 1 with its first two swapped (D3D strip
	// parity), each index offset by the base.
	const std::vector<std::uint16_t> strip = { 0, 1, 2, 3 };
	std::vector<std::uint32_t> list;
	triangle_strip_to_list(strip, 10, list);
	const std::vector<std::uint32_t> expected = { 10, 11, 12, 12, 11, 13 };
	CHECK(list == expected);
}

void check_degenerate_stitches_drop() {
	// A stitch (2, 2, 6, 6) joins two runs: every triangle with two equal
	// indices is dropped, and the survivors keep their own parity.
	const std::vector<std::uint16_t> strip = { 0, 1, 2, 2, 6, 6, 7, 8 };
	std::vector<std::uint32_t> list;
	triangle_strip_to_list(strip, 0, list);
	// Triangles 0..5: (0,1,2) even; (1,2,2), (2,2,6), (2,6,6), (6,6,7) dropped;
	// (6,7,8) at t = 5, odd: (7,6,8).
	const std::vector<std::uint32_t> expected = { 0, 1, 2, 7, 6, 8 };
	CHECK(list == expected);
}

void check_short_strips_and_appends() {
	std::vector<std::uint32_t> list = { 99 };
	triangle_strip_to_list(std::vector<std::uint16_t>{ 0, 1 }, 0, list);
	CHECK(list.size() == 1);
	triangle_strip_to_list(std::vector<std::uint16_t>{}, 0, list);
	CHECK(list.size() == 1);
	triangle_strip_to_list(std::vector<std::uint16_t>{ 4, 5, 6 }, 0, list);
	CHECK((list == std::vector<std::uint32_t>{ 99, 4, 5, 6 }));
}

void check_water_batch() {
	// A full five-row water batch (8 x 5 - 10 = 30 table entries): 28 strip
	// triangles, 12 of them the stitches between row pairs.
	std::vector<std::uint32_t> list;
	triangle_strip_to_list(opennova::env::kWaterStripIndexTable,
			std::size(opennova::env::kWaterStripIndexTable), 3, list);
	CHECK(list.size() % 3 == 0);
	CHECK(list.size() == 16u * 3u);
	CHECK(list[0] == 3u + 3u && list[1] == 3u + 0u && list[2] == 3u + 4u);
}

} // namespace

int main() {
	check_parity_and_base();
	check_degenerate_stitches_drop();
	check_short_strips_and_appends();
	check_water_batch();
	if (failures != 0) {
		std::printf("renderer_triangle_strip: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("renderer_triangle_strip ok\n");
	return 0;
}
