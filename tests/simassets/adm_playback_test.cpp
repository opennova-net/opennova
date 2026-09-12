#include <cmath>
#include <filesystem>
#include <fstream>
#include <vector>
#include "common/test_expect.h"
#include "common/test_paths.h"
#include <base/io/le.h>
#include <base/resource_index/resource_index.h>
#include <runtime/simassets/adm_root_motion.h>

namespace {
// Root-only BAD: ramps make the production loader's timing observable.
void write_clip(const std::filesystem::path &path, uint32_t fps, bool loop) {
	std::vector<uint8_t> bytes(80, 0);
	opennova::io::write_u32_le(bytes.data(), 1);
	opennova::io::write_u32_le(bytes.data() + 4, 80);
	opennova::io::write_u32_le(bytes.data() + 8, fps);
	opennova::io::write_u32_le(bytes.data() + 12, 60);
	opennova::io::write_u32_le(bytes.data() + 16, loop ? 1 : 0);
	opennova::io::write_u32_le(bytes.data() + 0x40, 80);
	opennova::io::write_u32_le(bytes.data() + 0x3C, 61);
	for (int i = 0; i <= 60; ++i) {
		for (int lane = 0; lane < 5; ++lane)
			opennova::io::append_f32_le(bytes, float(i + lane + 1));
		opennova::io::append_u32_le(bytes, uint32_t(i + 1));
	}
	std::ofstream file(path, std::ios::binary);
	file.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}
}

int main() {
	const auto dir = std::filesystem::path(test_paths_temp_dir()) / "opennova_adm_playback";
	std::filesystem::create_directories(dir);
	write_clip(dir / "once.bad", 30, false);
	write_clip(dir / "slow.bad", 24, true);
	{
		std::ofstream adm(dir / "clock.adm");
		adm << "anim_reset \"once\"\nanim_idle \"slow\"\n";
	}
	opennova::ResourceIndex index;
	TEST_EXPECT(index.scan(dir.string()));
	opennova::simassets::AdmRootMotion source;
	const int id = source.register_adm(&index, "clock.adm");
	TEST_EXPECT(id >= 0);
	using namespace opennova::world;
	int32_t phase = 0;
	RootMotionFrame frame{};
	TEST_EXPECT(source.advance(id, anim_state::kReset, phase, frame));
	const int32_t expected_forward = int32_t((3.0 + 30.0 / 62.0) * 32768.0);
	std::printf("tick 1: forward=%d, retail=%d\n", frame.dx, expected_forward);
	TEST_EXPECT(std::abs(frame.dx - expected_forward) <= 1);
	phase = 61;
	TEST_EXPECT(source.advance(id, anim_state::kIdle, phase, frame));
	TEST_EXPECT(std::abs(frame.dx - 27 * 32768) <= 2);
	// Still running after the old 120-tick cutoff.
	phase = 119;
	TEST_EXPECT(source.advance(id, anim_state::kReset, phase, frame));
	TEST_EXPECT(frame.dx != 0 && frame.events != 0);
	TEST_EXPECT(source.clip_length_ticks(id, anim_state::kReset, 0) == 125);
	phase = source.clip_length_ticks(id, anim_state::kReset, 0) - 1;
	TEST_EXPECT(source.advance(id, anim_state::kReset, phase, frame));
	TEST_EXPECT(frame.dx == 0 && frame.dy == 0 && frame.dz == 0 && frame.events == 0);
	TEST_EXPECT(std::abs(frame.capsule_bottom - int32_t((4.0 + 60.0 * 0.99999) * 65536)) <= 2);
	const int32_t parked_bottom = frame.capsule_bottom;
	TEST_EXPECT(source.advance(id, anim_state::kReset, phase, frame));
	TEST_EXPECT(frame.dx == 0 && frame.events == 0 && frame.capsule_bottom == parked_bottom);
	// Parked channels retain capsule extents while contributing zero velocity.
	int32_t target_phase = 0;
	TEST_EXPECT(source.advance_blended(id, anim_state::kReset, phase,
			anim_state::kIdle, target_phase, 0.5f, frame));
	TEST_EXPECT(std::abs(frame.dx - int32_t((3.0 + 24.0 / 62.0) * 16384.0)) <= 1);
	std::filesystem::remove(dir / "clock.adm");
	std::filesystem::remove(dir / "once.bad");
	std::filesystem::remove(dir / "slow.bad");
	std::filesystem::remove(dir);
	return 0;
}
