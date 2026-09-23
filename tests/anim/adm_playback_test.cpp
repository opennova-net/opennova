#include <cmath>
#include <filesystem>
#include <fstream>
#include <vector>
#include "common/test_expect.h"
#include "common/test_paths.h"
#include <base/io/le.h>
#include <base/resource_index/resource_index.h>
#include <runtime/assets/asset_store.h>
#include <net/npwire/ingame_encode.h>
#include <runtime/replication/client_replica_pipeline.h>
#include <runtime/anim/adm_root_motion.h>
#include <runtime/world/infantry_internal.h>

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

// One remote player row for the replica channel.
constexpr uint16_t kPlayerType = 0x14B9;
constexpr uint16_t kPlayerHandle = 0x0067;
opennova::EntityClass classify(uint16_t type_id) {
	return type_id == kPlayerType ? opennova::EntityClass::Player
	                              : opennova::EntityClass::Unknown;
}
std::vector<uint8_t> player_frame(uint8_t anim_state) {
	opennova::FrameUpdate fu;
	fu.anchor_x = 100 << 16;
	fu.anchor_y = 200 << 16;
	fu.anchor_z = 10 << 16;
	fu.flags2 = 0;
	fu.mount_handle = 0xFFFF;
	opennova::FrameUpdateRecord rec;
	rec.handle = kPlayerHandle;
	rec.type_id = kPlayerType;
	rec.cls = opennova::EntityClass::Player;
	rec.player.carrier_handle = 0xFFFF;
	rec.player.pos_x_compressed = opennova::network_compress_fixedpoint(1 << 16);
	rec.player.pos_y_compressed = opennova::network_compress_fixedpoint(2 << 16);
	rec.player.pos_z_compressed = opennova::network_compress_fixedpoint(0);
	rec.player.yaw_byte = 0x40;
	rec.player.anim_state_id = anim_state;
	rec.player.anim_channel_ratio = 0;
	rec.player.state_flags = 0;
	fu.records.push_back(rec);
	return opennova::encode_frame_update(fu);
}
}

int main() {
	const auto dir = std::filesystem::path(test_paths_temp_dir()) / "opennova_adm_playback";
	std::filesystem::create_directories(dir);
	write_clip(dir / "once.bad", 30, false);
	write_clip(dir / "slow.bad", 24, true);
	write_clip(dir / "frozen.bad", 0, true);
	{
		std::ofstream adm(dir / "clock.adm", std::ios::binary);
		adm << "anim_reset \"once\"\r\nanim_idle \"slow\"\r\nanim_walk_forward \"frozen\"\r\n";
	}
	opennova::ResourceIndex index;
	opennova::assets::AssetStore index_assets{&index};
	TEST_EXPECT(index.scan(dir.string()));
	opennova::anim::AdmRootMotion source;
	const int id = source.register_adm(&index_assets, "clock.adm");
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
	TEST_EXPECT(source.advance_blended(id, anim_state::kReset, 0, phase,
			anim_state::kIdle, 0, target_phase, 0.5f, frame));
	TEST_EXPECT(std::abs(frame.dx - int32_t((3.0 + 24.0 / 62.0) * 16384.0)) <= 1);

	// An fps of 0 is a channel frozen at frame 0 with live capsule extents, not a
	// missing clip: the delta is fps/62/frames with no fps test
	// [orig: AnimChannel_InitFromData @0x41058E..0x4105BA].
	TEST_EXPECT(source.has_clip(id, anim_state::kWalkForward));
	TEST_EXPECT(source.clip_length_ticks(id, anim_state::kWalkForward, 0) == -1);
	int32_t frozen = 0;
	for (int i = 0; i < 200; ++i) {
		TEST_EXPECT(source.advance(id, anim_state::kWalkForward, frozen, frame));
	}
	TEST_EXPECT(frame.dx == 3 * 32768 && frame.events == 1);
	TEST_EXPECT(frame.capsule_bottom == 4 * 65536 && frame.capsule_top == 5 * 65536 + 0x2000);

	// The armed-wrap park: a loop holding a deferred state wraps and is re-parked
	// at 0.99999 on its boundary tick (0x20000 latched, no 0x10000 stop), so that
	// tick samples the clip END -- the last frame's trigger and live velocity --
	// while the unarmed wrap samples the start; the promoted clip's frame 0 then
	// lands on the next tick [orig: AnimChannel_AdvancePlayback @0x40B193..0x40B1B1;
	// AnimMap_UpdateEntity @0x40B77B..0x40B7E1 before @0x40B7FE / @0x40B82A].
	const int32_t wrap = source.clip_boundary_after(id, anim_state::kIdle, 0, 0);
	TEST_EXPECT(wrap > 0);
	int32_t armed = wrap - 1;
	RootMotionFrame parked{};
	TEST_EXPECT(source.advance_armed(id, anim_state::kIdle, 0, armed, wrap, parked));
	TEST_EXPECT(armed == wrap);
	TEST_EXPECT(parked.events == 60);
	TEST_EXPECT(std::abs(parked.dx - int32_t((62.0 + (60.0 * 0.99999 - 59.0)) * 32768.0)) <= 2);
	TEST_EXPECT(std::abs(parked.capsule_bottom - int32_t((63.0 + (60.0 * 0.99999 - 59.0)) * 65536.0)) <= 2);
	int32_t unarmed = wrap - 1;
	RootMotionFrame wrapped{};
	TEST_EXPECT(source.advance(id, anim_state::kIdle, unarmed, wrapped));
	TEST_EXPECT(wrapped.events == 1 && wrapped.dx < parked.dx);
	// Off the boundary tick the armed advance is the plain advance.
	int32_t armed_before = wrap - 2;
	RootMotionFrame before{};
	TEST_EXPECT(source.advance_armed(id, anim_state::kIdle, 0, armed_before, wrap, before));
	int32_t plain_before = wrap - 2;
	RootMotionFrame plain{};
	TEST_EXPECT(source.advance(id, anim_state::kIdle, plain_before, plain));
	TEST_EXPECT(before.dx == plain.dx && before.events == plain.events);
	int32_t promoted = -1;
	RootMotionFrame first{};
	TEST_EXPECT(source.advance(id, anim_state::kReset, promoted, first));
	TEST_EXPECT(promoted == 0 && first.events == 1);

	// The consumers. The local primary channel: a queued state arms the
	// first-end tick (its step-3b promotion clock, clip_length_ticks), which
	// then samples the clip end; unarmed, that same tick wraps to the start.
	{
		opennova::world::InfantryState inf;
		inf.adm_id = id;
		inf.anim_state = anim_state::kIdle;
		inf.anim_pending = anim_state::kWalkForward;
		inf.clip_phase = wrap - 1;
		RootMotionFrame local{};
		opennova::world::AnimVariantRings rings;
		TEST_EXPECT(source.clip_length_ticks(id, anim_state::kIdle, 0) == wrap);
		TEST_EXPECT(opennova::world::advance_primary_channel(inf, source, rings, local));
		TEST_EXPECT(inf.clip_phase == wrap);
		TEST_EXPECT(local.events == 60 && local.capsule_bottom == parked.capsule_bottom);
		inf.anim_pending = 0;
		inf.clip_phase = wrap - 1;
		TEST_EXPECT(opennova::world::advance_primary_channel(inf, source, rings, local));
		TEST_EXPECT(local.events == 1 && local.capsule_bottom == wrapped.capsule_bottom);
	}
	// The replica channel: a deferral queued behind the looping idle arms its
	// next wrap lazily; that tick samples the parked end (the row's
	// capsule-bottom history holds it) and the promotion lands the tick after.
	{
		opennova::replication::ClientReplicaPipeline view;
		view.set_item_class_resolver(&classify);
		view.set_remote_motion_mode(true);
		view.set_root_motion_source(&source);
		view.state().upsert(kPlayerHandle).type_id = kPlayerType; // the spawn stream's row
		view.apply(0x0A, player_frame(static_cast<uint8_t>(anim_state::kIdle)));
		opennova::replication::ClientEntityState *es = view.state().find(kPlayerHandle);
		TEST_EXPECT(es != nullptr);
		es->rm_adm_id = static_cast<int16_t>(id);
		view.tick_remote_motion(0xFFFF);
		TEST_EXPECT(es->rm_state == anim_state::kIdle && es->rm_phase == 1);
		es->net_anim_pending = static_cast<int16_t>(anim_state::kWalkForward);
		es->net_anim_pending_boundary = -1;
		view.tick_remote_motion(0xFFFF);
		TEST_EXPECT(es->net_anim_pending_boundary == wrap);
		while (es->rm_phase < wrap - 1) view.tick_remote_motion(0xFFFF);
		view.tick_remote_motion(0xFFFF);
		TEST_EXPECT(es->rm_phase == wrap && es->net_anim_pending == anim_state::kWalkForward);
		TEST_EXPECT(es->rm_prev_bottom == parked.capsule_bottom);
		view.tick_remote_motion(0xFFFF);
		TEST_EXPECT(es->net_anim_pending == 0 &&
		            es->net_anim_current == anim_state::kWalkForward);
	}
	std::filesystem::remove(dir / "clock.adm");
	std::filesystem::remove(dir / "once.bad");
	std::filesystem::remove(dir / "slow.bad");
	std::filesystem::remove(dir / "frozen.bad");
	std::filesystem::remove(dir);
	return 0;
}
