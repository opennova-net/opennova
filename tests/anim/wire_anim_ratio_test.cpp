// The 0x0A player record's anim ratio byte over the body's primary channel: the playing
// channel's float t over its step, so a loop restarts the byte every pass and a one-shot holds
// it at its parked end; a cross-fade's weight over its step while one runs; a ratio past 255
// written 255. The oracles are retail's arithmetic worked outside the port: t accumulated in
// float (t += step, t -= 1 on a loop's wrap, 0.99999 on a one-shot's end), the quotient
// truncated [orig: NetPacket_SerializePlayerState @0x4C0CF2..0x4C0D35; AnimChannel_AdvancePlayback
// @0x40B140; AnimChannel_AdvanceBlendedPlayback @0x40B1E0]. Every pinned tick is clear of a
// truncation edge at 24, 53 and 64 bits: a quotient a few float ulps under an integer is
// pinned only where it sits many ulps under (the drifted passes), never one ulp under (Q3-11).
#include <filesystem>
#include <fstream>
#include <vector>

#include "common/test_expect.h"
#include "common/test_paths.h"
#include <base/io/le.h>
#include <base/resource_index/resource_index.h>
#include <runtime/anim/adm_root_motion.h>
#include <runtime/assets/asset_store.h>
#include <runtime/world/infantry_internal.h>

namespace {
// A root-only BAD of 60 frames at `fps`, looping or not (adm_playback_test's form).
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

using namespace opennova::world;

// Drives the primary channel `ticks` ticks.
bool run(InfantryState &inf, opennova::anim::AdmRootMotion &source, AnimVariantRings &rings,
         int ticks) {
	for (int i = 0; i < ticks; ++i) {
		RootMotionFrame frame{};
		if (!advance_primary_channel(inf, source, rings, frame)) return false;
	}
	return true;
}
} // namespace

int main() {
	const auto dir =
			std::filesystem::path(test_paths_temp_dir()) / test_paths_unique("opennova_wire_anim_ratio");
	std::filesystem::create_directories(dir);
	// slow: 24 fps, a pass of 156 ticks; long: 12 fps, 311 ticks; once: 30 fps, ends at 125.
	write_clip(dir / "slow.bad", 24, true);
	write_clip(dir / "long.bad", 12, true);
	write_clip(dir / "once.bad", 30, false);
	{
		std::ofstream adm(dir / "ratio.adm", std::ios::binary);
		adm << "anim_reset \"once\"\r\nanim_idle \"slow\"\r\nanim_walk_forward \"long\"\r\n";
	}
	opennova::ResourceIndex index;
	opennova::assets::AssetStore assets{&index};
	TEST_EXPECT(index.scan(dir.string()));
	opennova::anim::AdmRootMotion source;
	const int id = source.register_adm(&assets, "ratio.adm");
	TEST_EXPECT(id >= 0);
	AnimVariantRings rings;

	// A loop past its first pass: the byte restarts at the wrap. The drifted float t sits
	// under n * step, so the byte reads one under the tick count from the first pass's middle
	// on (the elapsed-tick byte read 100, 155, 156, 200, 255).
	{
		InfantryState inf;
		inf.adm_id = id;
		inf.anim_state = anim_state::kIdle;
		TEST_EXPECT(run(inf, source, rings, 1));
		TEST_EXPECT(wire_anim_ratio(inf, &source) == 1);
		TEST_EXPECT(run(inf, source, rings, 99)); // tick 100
		TEST_EXPECT(wire_anim_ratio(inf, &source) == 99);
		TEST_EXPECT(run(inf, source, rings, 55)); // tick 155, the pass's last
		TEST_EXPECT(wire_anim_ratio(inf, &source) == 154);
		TEST_EXPECT(run(inf, source, rings, 1)); // tick 156: t wrapped
		TEST_EXPECT(inf.clip_phase == 156);
		TEST_EXPECT(wire_anim_ratio(inf, &source) == 0);
		TEST_EXPECT(run(inf, source, rings, 44)); // tick 200
		TEST_EXPECT(wire_anim_ratio(inf, &source) == 44);
		TEST_EXPECT(run(inf, source, rings, 111)); // tick 311: the second wrap
		TEST_EXPECT(wire_anim_ratio(inf, &source) == 0);
		TEST_EXPECT(run(inf, source, rings, 9)); // tick 320
		TEST_EXPECT(wire_anim_ratio(inf, &source) == 9);
	}

	// A pass longer than 255 ticks: the byte caps at 255 to the wrap, then restarts.
	{
		InfantryState inf;
		inf.adm_id = id;
		inf.anim_state = anim_state::kWalkForward;
		TEST_EXPECT(run(inf, source, rings, 200));
		TEST_EXPECT(wire_anim_ratio(inf, &source) == 199);
		TEST_EXPECT(run(inf, source, rings, 56)); // tick 256: 255.9998, truncated
		TEST_EXPECT(wire_anim_ratio(inf, &source) == 255);
		TEST_EXPECT(run(inf, source, rings, 44)); // tick 300: 299, capped
		TEST_EXPECT(wire_anim_ratio(inf, &source) == 255);
		TEST_EXPECT(run(inf, source, rings, 11)); // tick 311: wrapped
		TEST_EXPECT(wire_anim_ratio(inf, &source) == 0);
		TEST_EXPECT(run(inf, source, rings, 9)); // tick 320
		TEST_EXPECT(wire_anim_ratio(inf, &source) == 9);
	}

	// A one-shot past its end: t parks at 0.99999, the byte holds at 0.99999 / step.
	{
		InfantryState inf;
		inf.adm_id = id;
		inf.anim_state = anim_state::kReset;
		TEST_EXPECT(run(inf, source, rings, 200));
		TEST_EXPECT(wire_anim_ratio(inf, &source) == 123);
	}

	// An armed loop on its boundary tick parks at 0.99999 too: the byte reads the pass's end,
	// not the wrapped start.
	{
		InfantryState inf;
		inf.adm_id = id;
		inf.anim_state = anim_state::kIdle;
		inf.anim_pending = anim_state::kWalkForward;
		inf.clip_phase = 155;
		TEST_EXPECT(run(inf, source, rings, 1));
		TEST_EXPECT(inf.clip_phase == 156 && inf.body_phase_parked());
		TEST_EXPECT(wire_anim_ratio(inf, &source) == 154);
	}

	// A cross-fade: the weight over the step, the ticks it has run (step 0.1 into idle, whose
	// flag word lacks 0x400); at its tenth tick the fade ends and the incoming channel's own
	// t / step follows.
	{
		InfantryState inf;
		inf.adm_id = id;
		inf.anim_state = anim_state::kWalkForward;
		TEST_EXPECT(run(inf, source, rings, 30));
		inf.request_body_animation(anim_state::kIdle);
		// Not the fifth: 0.5f / 0.1f is 4.99999992, one float ulp under 5 (Q3-11).
		const int expected[] = {1, 2, 3, 4, -1, 6, 7, 8, 9};
		for (int n = 1; n <= 9; ++n) {
			TEST_EXPECT(run(inf, source, rings, 1));
			TEST_EXPECT(inf.body_blend_active());
			if (expected[n - 1] >= 0) TEST_EXPECT(wire_anim_ratio(inf, &source) == expected[n - 1]);
		}
		TEST_EXPECT(run(inf, source, rings, 1));
		TEST_EXPECT(!inf.body_blend_active() && inf.clip_phase == 10);
		TEST_EXPECT(wire_anim_ratio(inf, &source) == 10);
	}

	// A provider without the retail clock keeps the elapsed ticks.
	{
		InfantryState inf;
		inf.clip_phase = 300;
		TEST_EXPECT(wire_anim_ratio(inf, nullptr) == 255);
		inf.clip_phase = 37;
		TEST_EXPECT(wire_anim_ratio(inf, nullptr) == 37);
	}
	std::filesystem::remove_all(dir);
	return 0;
}
