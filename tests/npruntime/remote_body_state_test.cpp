// The remote body-state arbitration over a real clip set: a wire remote
// player lying prone (48 idle_prone) taps a roll (41 roll_left, a hold-class
// state that LOCKS); the follow-up roll_right (42) QUEUES behind the lock
// [orig: the queue classes @0x4c1169..0x4c1190] and promotes at the roll
// clip's completion boundary, measured against the .adm's real clip length
// through AdmRootMotion (a looping clip's boundary is its next loop end; from
// phase 0 that is one clip length either way). The synthetic sibling
// (netsim_client_replica_pipeline_body_arbitration) pins the rule with a fake
// clip source; this one runs the loaded clips. The set is minted here in the
// shape of the shipped US01.adm's (Dt1Prone 60 frames, Dt1RollL/R 30 frames,
// all 30 fps and looping).
#include "common/file_io.h"
#include "common/temp_dir.h"

#include <formats/bad/bad_build.h>

#include <base/resource_index/resource_index.h>
#include <base/vfs/vfs.h>
#include <runtime/replication/client_replica_pipeline.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <runtime/anim/adm_root_motion.h>
#include <runtime/assets/asset_store.h>
#include <runtime/world/entity.h>
#include <runtime/world/infantry.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace opennova;
namespace ns = opennova::replication;
namespace as = opennova::world::anim_state;

int failures = 0;
bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	++failures;
	return false;
}

constexpr uint16_t kPlayerType = 0x14B9;
constexpr uint16_t kPlayerHandle = 0x0067;

EntityClass classify(uint16_t type_id) {
	return type_id == kPlayerType ? EntityClass::Player : EntityClass::Unknown;
}

std::vector<uint8_t> player_frame(uint8_t anim_state, uint8_t ratio) {
	FrameUpdate fu;
	fu.anchor_x = 100 << 16;
	fu.anchor_y = 200 << 16;
	fu.anchor_z = 10 << 16;
	fu.flags2 = 0;
	fu.carried_handle = 0xFFFF;
	FrameUpdateRecord rec;
	rec.handle = kPlayerHandle;
	rec.type_id = kPlayerType;
	rec.cls = EntityClass::Player;
	rec.player.carrier_handle = 0xFFFF;
	rec.player.pos_x_compressed = network_compress_fixedpoint(1 << 16);
	rec.player.pos_y_compressed = network_compress_fixedpoint(2 << 16);
	rec.player.pos_z_compressed = network_compress_fixedpoint(0);
	rec.player.yaw_byte = 0x40;
	rec.player.anim_state_id = anim_state;
	rec.player.anim_channel_ratio = ratio;
	rec.player.state_flags = 0;
	fu.records.push_back(rec);
	return encode_frame_update(fu);
}

// A one-bone looping clip at 30 fps over `frames` intervals.
bool write_clip(const std::string &path, const char *stem, uint32_t frames) {
	bad::BadBuildClip clip;
	clip.name = stem;
	clip.fps = 30;
	clip.flags = 0x1; // loop
	clip.frame_count = frames;
	bad::BadBuildBone bone;
	bone.name = "BN01";
	bone.keys.assign(frames + 1, bad::BadBuildQuat{});
	clip.bones.push_back(bone);
	// The root records the runtime reads a clip's length from (one per frame
	// and the extra end record), a prone body's capsule.
	bad::BadBuildEvent event;
	event.bottom = 0.16;
	event.top = 0.24;
	clip.events.assign(frames + 1, event);
	std::vector<uint8_t> bytes;
	std::string error;
	return bad::bad_build_mint(clip, nullptr, bad::bad_retail_limits(), bytes, &error) &&
	       test_io::write_file(path, bytes);
}

} // namespace

int main() {
	test_temp::TempDir dir("remote_body_state");
	const std::string root = dir.root();
	if (!expect(write_clip(root + "/rbrst.bad", "rbrst", 1) && write_clip(root + "/rbprone.bad", "rbprone", 60) &&
						write_clip(root + "/rbrolll.bad", "rbrolll", 30) &&
						write_clip(root + "/rbrollr.bad", "rbrollr", 30) &&
						test_io::write_text(root + "/rb.adm",
								"anim_reset \"rbrst.bad\"\r\n"
								"anim_idle_prone \"rbprone.bad\"\r\n"
								"anim_roll_left \"rbrolll.bad\"\r\n"
								"anim_roll_right \"rbrollr.bad\"\r\n"),
				"the clip set is minted"))
		return 1;
	ResourceIndex index;
	if (!expect(index.scan(root, std::string(), VfsMountMode::LooseOnly), "the clip set mounts")) return 1;
	opennova::assets::AssetStore store{&index};
	anim::AdmRootMotion root_motion;
	const int adm = root_motion.register_adm(&store, "rb.adm");
	if (!expect(adm >= 0, "rb.adm registers")) return 1;

	expect(std::strcmp(anim::kAnimSlotNames[as::kRollLeft], "roll_left") == 0,
			"state 41 maps to roll_left");
	expect(std::strcmp(anim::kAnimSlotNames[as::kRollRight], "roll_right") == 0,
			"state 42 maps to roll_right");
	expect(root_motion.has_clip(adm, as::kIdleProne), "the set carries anim_idle_prone");
	expect(root_motion.has_clip(adm, as::kRollLeft), "the set carries anim_roll_left");
	expect(root_motion.has_clip(adm, as::kRollRight), "the set carries anim_roll_right");
	const int32_t roll_len = root_motion.clip_length_ticks(adm, as::kRollLeft, 0);
	std::printf("remote_body_state: roll_left %d ticks (loops=%d), idle_prone %d ticks (loops=%d)\n",
			roll_len, int(root_motion.clip_loops(adm, as::kRollLeft, 0)),
			root_motion.clip_length_ticks(adm, as::kIdleProne, 0),
			int(root_motion.clip_loops(adm, as::kIdleProne, 0)));
	expect(roll_len > 0, "roll_left has a length");
	expect((world::infantry_anim_flags(as::kRollLeft) & 0x4u) != 0u,
			"roll_left is a hold-class (locking) state");

	ns::ClientReplicaPipeline view;
	view.set_item_class_resolver(&classify);
	view.set_remote_motion_mode(true);
	view.set_root_motion_source(&root_motion);
	view.state().upsert(kPlayerHandle).type_id = kPlayerType; // the spawn stream's row
	view.apply(0x0A, player_frame(as::kIdleProne, 0));
	ns::ClientEntityState *es = view.state().find(kPlayerHandle);
	if (!expect(es != nullptr, "the player row decoded")) return 1;
	es->rm_adm_id = static_cast<int16_t>(adm);
	view.tick_remote_motion(0xFFFF);
	expect(es->rm_state == as::kIdleProne, "the channel arms on idle_prone");
	expect(es->net_anim_current == as::kIdleProne, "idle_prone is current");

	view.apply(0x0A, player_frame(as::kRollLeft, 0));
	expect(es->net_anim_current == as::kRollLeft, "roll_left commits directly and locks");
	view.apply(0x0A, player_frame(as::kRollRight, 0));
	expect(es->net_anim_current == as::kRollLeft, "roll_right does not replace the locked roll");
	expect(es->net_anim_pending == as::kRollRight, "roll_right queues behind the locked roll_left");
	view.tick_remote_motion(0xFFFF);
	expect(es->rm_state == as::kRollLeft, "the roll plays");

	int promoted_at = -1;
	for (int t = 1; t <= roll_len + 4; ++t) {
		view.tick_remote_motion(0xFFFF);
		if (es->net_anim_current == as::kRollRight) {
			promoted_at = t;
			break;
		}
	}
	std::printf("remote_body_state: roll_right promoted after %d ticks of the %d-tick roll\n",
			promoted_at, roll_len);
	expect(promoted_at > 0, "the queued roll_right promotes at the roll's completion boundary");
	expect(promoted_at == roll_len, "...exactly one roll_left clip length after the retarget");
	expect(es->net_anim_pending == 0, "...consuming the pending");
	view.tick_remote_motion(0xFFFF);
	expect(es->rm_state == as::kRollRight, "the promoted retarget lands on the following tick");

	if (failures == 0) std::printf("remote_body_state: OK\n");
	return failures == 0 ? 0 : 1;
}
