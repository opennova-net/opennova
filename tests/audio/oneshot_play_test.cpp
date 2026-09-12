// runtime/audio/oneshot_play: the case-insensitive first-bank-wins set index
// and the 3D one-shot fire decision chain -- the set-range cull (equality
// passes), the occlusion inflate + recheck, the per-layer member pick through
// the selector and the once-at-fire volume -- over synthetic lwf::File banks.
// The distance curve itself is pinned by ambient_mixer_test; here only the
// pinned half-range byte (63 at d = r/2) is re-read through the plan.

#include <runtime/audio/oneshot_play.h>

#include "common/test_expect.h"

#include <string>
#include <vector>

namespace {

using namespace opennova;
using namespace opennova::audio;

// One single (`wav`) shared by every member; one sndparm per (volume, clamp);
// one playlist per layer; one multi per set. Indices are appended in order.
struct BankBuilder {
	lwf::File file;

	BankBuilder() {
		lwf::Single single;
		single.name = "tone";
		single.path = "tone.wav";
		file.singles.push_back(single);
	}

	uint32_t member(uint32_t volume, uint32_t clamp) {
		lwf::Sndparm sp;
		sp.single_index = 0;
		sp.pitch_scaled = lwf::kPitchUnityQ16;
		sp.volume = volume;
		sp.clamp_volume = clamp;
		file.sndparms.push_back(sp);
		return static_cast<uint32_t>(file.sndparms.size() - 1);
	}

	uint32_t layer(uint16_t falloff, uint16_t min_distance, uint32_t flags,
			std::vector<uint32_t> members) {
		lwf::Playlist pl;
		pl.falloff_radius = falloff;
		pl.min_distance = min_distance;
		pl.flags = flags;
		pl.sndparm_indices = std::move(members);
		file.playlists.push_back(pl);
		return static_cast<uint32_t>(file.playlists.size() - 1);
	}

	int32_t set(const std::string &name, uint32_t target_id, std::vector<uint32_t> layers) {
		lwf::Multi m;
		m.name = name;
		m.target_id = target_id;
		m.playlist_indices = std::move(layers);
		file.multis.push_back(m);
		return static_cast<int32_t>(file.multis.size() - 1);
	}
};

struct OcclusionProbe {
	int calls = 0;
	int64_t last_source = 0;
	int64_t inflated_q16 = -1; // < 0: pass the raw distance through

	static int64_t fn(void *ctx, const float[3], const float[3], int64_t dist_q16,
			int64_t source_id) {
		OcclusionProbe *self = static_cast<OcclusionProbe *>(ctx);
		++self->calls;
		self->last_source = source_id;
		return self->inflated_q16 < 0 ? dist_q16 : self->inflated_q16;
	}
};

const float kOrigin[3] = { 0.0f, 0.0f, 0.0f };

int test_index_is_case_insensitive_and_first_bank_wins() {
	BankBuilder mission;
	mission.set("Z00AMB1", 0, {});
	mission.set("Shared", 0, {});
	BankBuilder global;
	global.set("shared", 0, {});
	global.set("LPNV_LIGHT", 0, {});

	SoundSetIndex index;
	index.add_bank(0, mission.file);
	index.add_bank(1, global.file);
	TEST_EXPECT(index.has("Z00AMB1"));
	TEST_EXPECT(index.has("z00amb1"));
	TEST_EXPECT(!index.has("NOPE"));
	TEST_EXPECT(index.find("z00amb1").bank == 0 && index.find("z00amb1").set == 0);
	TEST_EXPECT(index.find("SHARED").bank == 0 && index.find("SHARED").set == 1);
	TEST_EXPECT(index.find("lpnv_light").bank == 1 && index.find("lpnv_light").set == 1);
	TEST_EXPECT(!index.find("nope").valid());
	const std::vector<std::string> expect_names = { "z00amb1", "shared", "lpnv_light" };
	TEST_EXPECT(index.names() == expect_names);
	index.clear();
	TEST_EXPECT(index.names().empty() && !index.has("Z00AMB1"));
	return 0;
}

int test_distance_and_cull_range() {
	const float one_unit[3] = { 1.0f, 0.0f, 0.0f };
	TEST_EXPECT(listener_distance_q16(one_unit, kOrigin) == 65536);
	TEST_EXPECT(listener_distance_q16(kOrigin, kOrigin) == 0);
	lwf::Multi set;
	set.target_id = 200;
	TEST_EXPECT(oneshot_cull_range_q16(set) == (200LL << 16));
	set.target_id = 0;
	TEST_EXPECT(oneshot_cull_range_q16(set) == 0);
	return 0;
}

int test_zero_range_fires_only_at_the_exact_source() {
	BankBuilder b;
	const uint32_t m = b.member(255, 255);
	const uint32_t l = b.layer(200, 0, 0, { m });
	b.set("POINT_ONLY", 0, { l });
	SoundSetIndex index;
	index.add_bank(0, b.file);
	SoundSelector selector;
	const SetLocation loc = index.find("POINT_ONLY");

	const float one_unit[3] = { 1.0f, 0.0f, 0.0f };
	const OneshotPlan far = plan_oneshot_3d(b.file, loc, one_unit, kOrigin, true, 0,
			nullptr, nullptr, selector);
	TEST_EXPECT(!far.in_range && far.voices.empty());
	const OneshotPlan at = plan_oneshot_3d(b.file, loc, kOrigin, kOrigin, true, 0,
			nullptr, nullptr, selector);
	TEST_EXPECT(at.in_range && at.voices.size() == 1);
	return 0;
}

int test_occlusion_inflates_the_fire_distance() {
	// Raw distance 50u; the two-ray result inflates it to 100u. With a 200u
	// falloff that is the pinned half-range volume byte 63.
	BankBuilder b;
	const uint32_t m = b.member(255, 255);
	const uint32_t l = b.layer(200, 0, 0, { m });
	b.set("OCCLUDED", 200, { l });
	SoundSetIndex index;
	index.add_bank(0, b.file);
	SoundSelector selector;
	OcclusionProbe probe;
	probe.inflated_q16 = 100LL << 16;
	const float source[3] = { 50.0f, 0.0f, 0.0f };
	const OneshotPlan plan = plan_oneshot_3d(b.file, index.find("occluded"), source,
			kOrigin, true, 321, &OcclusionProbe::fn, &probe, selector);
	TEST_EXPECT(probe.calls == 1);
	TEST_EXPECT(probe.last_source == 321);
	TEST_EXPECT(plan.in_range);
	TEST_EXPECT(plan.dist_q16 == (100LL << 16));
	TEST_EXPECT(plan.voices.size() == 1);
	TEST_EXPECT(plan.voices[0].vol255 == 63);
	TEST_EXPECT(plan.voices[0].layer == 0 && plan.voices[0].playlist == l &&
			plan.voices[0].sndparm == m);
	return 0;
}

int test_occlusion_recheck_rejects_the_inflated_distance() {
	// Raw 100u passes the set's 120u cull; occlusion inflates it to 130u, so
	// the post-LOS range recheck rejects the fire after one query.
	BankBuilder b;
	const uint32_t m = b.member(255, 255);
	const uint32_t l = b.layer(200, 0, 0, { m });
	b.set("OCCLUDED_CULL", 120, { l });
	SoundSetIndex index;
	index.add_bank(0, b.file);
	SoundSelector selector;
	OcclusionProbe probe;
	probe.inflated_q16 = 130LL << 16;
	const float source[3] = { 100.0f, 0.0f, 0.0f };
	const OneshotPlan plan = plan_oneshot_3d(b.file, index.find("OCCLUDED_CULL"), source,
			kOrigin, true, 0, &OcclusionProbe::fn, &probe, selector);
	TEST_EXPECT(probe.calls == 1);
	TEST_EXPECT(!plan.in_range && plan.voices.empty());
	return 0;
}

int test_no_listener_plays_the_member_volume_flat() {
	BankBuilder b;
	const uint32_t quiet = b.member(100, 255);
	const uint32_t l = b.layer(200, 0, 0, { quiet });
	b.set("FLAT", 0, { l });
	SoundSetIndex index;
	index.add_bank(0, b.file);
	SoundSelector selector;
	OcclusionProbe probe;
	const float source[3] = { 500.0f, 0.0f, 0.0f };
	const OneshotPlan plan = plan_oneshot_3d(b.file, index.find("FLAT"), source, kOrigin,
			false, 0, &OcclusionProbe::fn, &probe, selector);
	TEST_EXPECT(probe.calls == 0);
	TEST_EXPECT(plan.in_range && plan.dist_q16 == 0);
	TEST_EXPECT(plan.voices.size() == 1 && plan.voices[0].vol255 == 100);
	return 0;
}

int test_silent_layers_drop_and_every_layer_picks() {
	// Layer 0 is out of its 50u falloff at 100u (silent, dropped); layer 1
	// (2000u) plays. Layer 0's sequential cursor still advanced: the next
	// fire inside range plays its SECOND member.
	BankBuilder b;
	const uint32_t a0 = b.member(255, 255);
	const uint32_t a1 = b.member(200, 255);
	const uint32_t bm = b.member(255, 255);
	const uint32_t near = b.layer(50, 0, lwf::kFlagSequential, { a0, a1 });
	const uint32_t wide = b.layer(2000, 0, 0, { bm });
	b.set("TWO", 5000, { near, wide });
	SoundSetIndex index;
	index.add_bank(0, b.file);
	SoundSelector selector;
	const SetLocation loc = index.find("two");

	const float far[3] = { 100.0f, 0.0f, 0.0f };
	const OneshotPlan first = plan_oneshot_3d(b.file, loc, far, kOrigin, true, 0, nullptr,
			nullptr, selector);
	TEST_EXPECT(first.in_range);
	TEST_EXPECT(first.voices.size() == 1);
	TEST_EXPECT(first.voices[0].layer == 1 && first.voices[0].sndparm == bm);

	const OneshotPlan second = plan_oneshot_3d(b.file, loc, kOrigin, kOrigin, true, 0,
			nullptr, nullptr, selector);
	TEST_EXPECT(second.voices.size() == 2);
	TEST_EXPECT(second.voices[0].layer == 0 && second.voices[0].sndparm == a1);
	TEST_EXPECT(second.voices[1].layer == 1 && second.voices[1].sndparm == bm);
	return 0;
}

int test_direct_distance_skips_set_cull_but_retains_layer_gain_and_selection() {
    BankBuilder b;
    const uint32_t a = b.member(255, 255);
    const uint32_t next = b.member(120, 255);
    const uint32_t first = b.layer(200, 0, lwf::kFlagSequential, {a, next});
    const uint32_t second = b.layer(50, 0, 0, {a});
    b.set("DIRECT", 1, {first, second});
    SoundSetIndex index; index.add_bank(0, b.file);
    SoundSelector selector;
    const auto loc = index.find("DIRECT");
    const float far[3] = {100, 0, 0};
    OcclusionProbe probe;
    const auto positional = plan_oneshot_3d(b.file, loc, far, kOrigin, true, 0,
            &OcclusionProbe::fn, &probe, selector);
    TEST_EXPECT(!positional.in_range && probe.calls == 0);
    const auto direct = plan_oneshot_at_distance(b.file, loc, 100LL << 16, selector);
    TEST_EXPECT(direct.in_range && direct.dist_q16 == (100LL << 16));
    TEST_EXPECT(direct.voices.size() == 1);
    TEST_EXPECT(direct.voices[0].sndparm == a && direct.voices[0].vol255 == 63);
    // A culled positional fire did not consume the cursor. Direct fires do,
    // including layers whose own distance law makes them silent.
    const auto nearby = plan_oneshot_at_distance(b.file, loc, 0, selector);
    TEST_EXPECT(nearby.voices.size() == 2);
    TEST_EXPECT(nearby.voices[0].sndparm == next);
    TEST_EXPECT(!plan_oneshot_at_distance(b.file, {}, 0, selector).in_range);
    TEST_EXPECT(plan_oneshot_at_distance(b.file, loc, 200LL << 16, selector).voices.empty());
    return 0;
}

int test_member_pick_skips_dangling_indices() {
	// An out-of-range sndparm index is dropped from the layer (the format
	// reader's tree does the same), so the selector sees the carried count.
	BankBuilder b;
	const uint32_t m0 = b.member(255, 255);
	const uint32_t m1 = b.member(255, 255);
	const uint32_t l = b.layer(200, 0, lwf::kFlagSequential, { m0, 99, m1 });
	b.set("DANGLING", 0, { l, 77 });
	SoundSetIndex index;
	index.add_bank(0, b.file);
	SoundSelector selector;
	const SetLocation loc = index.find("DANGLING");
	TEST_EXPECT(set_layers(b.file, b.file.multis[0]).size() == 1);
	TEST_EXPECT(layer_members(b.file, b.file.playlists[l]).size() == 2);
	TEST_EXPECT(pick_layer_member(b.file, loc, 0, l, selector) == 0);
	TEST_EXPECT(pick_layer_member(b.file, loc, 0, l, selector) == 1);
	TEST_EXPECT(pick_layer_member(b.file, loc, 0, l, selector) == 0);
	TEST_EXPECT(pick_layer_member(b.file, loc, 0, 77, selector) == -1);
	const uint32_t empty = b.layer(200, 0, 0, {});
	TEST_EXPECT(pick_layer_member(b.file, loc, 1, empty, selector) == -1);
	return 0;
}

} // namespace

int test_pitch_draws_and_channel_pool() {
	BankBuilder bank;
	const auto member = bank.member(255, 255);
	bank.file.sndparms[member].random_pitch_scaled = 8192;
	const auto layer = bank.layer(200, 0, 0, {member});
	bank.set("PITCH", 200, {layer});
	bank.file.multis[0].pitch_base = 98304;
	bank.file.multis[0].pitch_random_range = 32768;
	SoundSelector selector;
	const auto plan = plan_oneshot_at_distance(bank.file, {0, 0}, 0, selector);
	TEST_EXPECT(plan.voices.size() == 1 && plan.voices[0].pitch_q16 == 110686u);
	TEST_EXPECT(selector.select(99, 256, kRandom) == 229);
	SoundSelector zero_ranges;
	TEST_EXPECT(zero_ranges.compose_pitch(65536, 0, 65536, 0) == 65536u);
	TEST_EXPECT(zero_ranges.select(99, 256, kRandom) == 249);
	OneshotChannelPool pool;
	for (int slot = 12; slot < 26; ++slot)
		TEST_EXPECT(pool.acquire(100 + slot, 200) == slot);
	TEST_EXPECT(pool.acquire(999, 100) == -1); // exactly half cannot steal
	TEST_EXPECT(pool.acquire(999, 101) == 12); // strictly louder than half can
	TEST_EXPECT(pool.acquire(999, 0) == -1);
	pool.release(20);
	TEST_EXPECT(pool.acquire(1000, 1) == 20); // idle outranks a quieter live slot
	OneshotChannelPool keyed;
	TEST_EXPECT(keyed.acquire(1234, 250, 9) == 12);
	TEST_EXPECT(keyed.acquire(1234, 1, 9) == 12); // same wave + nonzero id forces score zero
	TEST_EXPECT(keyed.acquire(1234, 1, 10) == 13);
	return 0;
}

int test_radio_selection_keeps_unity_pitch_and_gates_view_layers() {
    BankBuilder b;
    const auto first = b.member(180, 0);
    const auto third = b.member(220, 0);
    const auto fp = b.layer(50, 0, 2, {first});
    const auto tp = b.layer(80, 0, 4, {third});
    const int set = b.set("radio", 0, {fp, tp});
    b.file.multis[set].pitch_base = 98304;
    b.file.multis[set].pitch_random_range = 32768;
    SoundSelector selector;
    const auto selected = select_radio_voice(b.file, {0, set}, selector, 4);
    TEST_EXPECT(selected && selected->volume == 220 && selected->max_distance == 80 * 65536);
    TEST_EXPECT(selector.select(99, 256, kRandom) == 3); // one pick, no pitch draws
    b.file.multis[set].set_flags = 1;
    b.file.playlists[tp].flags |= 0x20;
    TEST_EXPECT(!select_radio_voice(b.file, {0, set}, selector, 4));
    return 0;
}

int main() {
	int failed = test_radio_selection_keeps_unity_pitch_and_gates_view_layers();
	failed |= test_index_is_case_insensitive_and_first_bank_wins();
	failed |= test_pitch_draws_and_channel_pool();
	failed |= test_distance_and_cull_range();
	failed |= test_zero_range_fires_only_at_the_exact_source();
	failed |= test_occlusion_inflates_the_fire_distance();
	failed |= test_occlusion_recheck_rejects_the_inflated_distance();
	failed |= test_no_listener_plays_the_member_volume_flat();
	failed |= test_silent_layers_drop_and_every_layer_picks();
	failed |= test_member_pick_skips_dangling_indices();
    failed |= test_direct_distance_skips_set_cull_but_retains_layer_gain_and_selection();
	if (failed) {
		return 1;
	}
	std::printf("oneshot_play_test: OK\n");
	return 0;
}
