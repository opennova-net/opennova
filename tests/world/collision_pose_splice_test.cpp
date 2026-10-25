// The weapon-channel splice in the collision / anchor pose (L5-s39 FOUND): every body the
// World poses splices its own secondary channel's mask bones when it carries the Player bit,
// sits in no control / gunner / driver seat and its requested state carries flag 0x40. A
// host's remote player takes it as the local one does, armed or not; a seated or flag-less
// body does not, and neither does a body without the bit. A joiner's seated player row takes
// the same seat test.
// [orig: Entity_BuildBoneTransformMatrices @0x4B1290 - the seat local @0x4B12A2..0x4B1347,
//  `test dword ptr [ebp+24h], 100h` @0x4B14A7, the seat test @0x4B14B9,
//  `test byte ptr g_AnimStateFlagsTable[edx*4], 40h` on +0x2BC @0x4B14C9, the body's own
//  +0x18C channel @0x4B134E posed by AnimChannel_ComputeBoneMatrices @0x4B16A7]
// The rig is the minted 19-bone person (common/rig19_set.h); its weapon channel plays the
// rig19 reload clip. The oracle is the type leg's composition (build_skeletal_from_inputs)
// over the same rig with the channel on and off.
#include "common/rig19_set.h"
#include "common/temp_dir.h"

#include <base/resource_index/resource_index.h>
#include <base/vfs/vfs.h>
#include <net/npwire/entity_class.h>
#include <runtime/assets/asset_store.h>
#include <runtime/inmatch/client_replica_present_projection.h>
#include <runtime/mission/promote.h>
#include <runtime/replication/client_state.h>
#include <runtime/world/ai.h>
#include <runtime/world/collision.h>
#include <runtime/world/entity.h>
#include <runtime/world/entity_pose.h>
#include <runtime/world/infantry.h>
#include <runtime/world/pose_inputs.h>
#include <runtime/world/pose_provider.h>
#include <runtime/world/present_rows.h>
#include <runtime/world/vehicle_mount.h>
#include <runtime/world/weapon_table.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace opennova;

namespace {

namespace w = opennova::world;
namespace as = opennova::world::anim_state;

int failures = 0;
bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	++failures;
	return false;
}

constexpr int32_t kModelId = 1;
constexpr uint16_t kPersonType = 7;
constexpr const char *kAdm = "rig19.adm";

bool same(const std::vector<w::CollisionMatrix> &a, const std::vector<w::CollisionMatrix> &b) {
	if (a.size() != b.size()) return false;
	for (size_t i = 0; i < a.size(); ++i)
		if (std::memcmp(a[i].m, b[i].m, sizeof(a[i].m)) != 0) return false;
	return true;
}

// The summed word distance between two section sets (an ordering, not a metric).
int64_t distance(const std::vector<w::CollisionMatrix> &a,
		const std::vector<w::CollisionMatrix> &b) {
	int64_t sum = 0;
	for (size_t i = 0; i < a.size() && i < b.size(); ++i)
		for (int k = 0; k < 16; ++k)
			sum += std::llabs(static_cast<int64_t>(a[i].m[k]) - static_cast<int64_t>(b[i].m[k]));
	return sum;
}

// The body's channels as the composition reads them, the secondary channel on or off.
w::SkeletalPoseInputs inputs_for(const w::AiEntity &ae, const w::Entity &e, bool weapon) {
	w::SkeletalPoseInputs in;
	in.primary_state = ae.inf.body_clip_state();
	in.primary_phase = ae.inf.clip_phase;
	in.primary_variant = ae.inf.anim_variant;
	in.primary_parked = ae.inf.body_phase_parked();
	in.overlay = w::aim_overlay_inputs_for(ae, e);
	in.collapse_right_hand = w::mount_collapses_right_hand_row(e);
	if (weapon) {
		in.weapon = true;
		in.weapon_state = ae.inf.weapon_clip_state();
		in.weapon_phase = ae.inf.wpn_clip_phase;
		in.weapon_variant = ae.inf.wpn_variant;
		in.weapon_parked = ae.inf.weapon_phase_parked();
	}
	return in;
}

struct Scene {
	test_temp::TempDir root{"opennova_collision_pose_splice"};
	ResourceIndex index;
	std::unique_ptr<assets::AssetStore> store;
	assets::Model model;
	std::unique_ptr<w::World> owner = std::make_unique<w::World>();
	w::World &world = *owner;
	w::EntityPoseProvider pose;
	w::CollisionModel sections;
	w::CollisionMatrix entity_world;
	w::EntityHandle body;
	bool ok = false;

	Scene() {
		if (!expect(rig19::stage(root.root()), "the rig stages")) return;
		if (!expect(index.scan(root.root(), std::string(), VfsMountMode::LooseOnly),
					"the staged rig mounts"))
			return;
		store = std::make_unique<assets::AssetStore>(&index);
		model = store->model("person");
		if (!expect(model != nullptr, "person.3di loads")) return;
		pose.set_assets(store.get());
		world.registry.configure_pool(0, 8);
		w::Entity seed;
		seed.kind = w::EntityKind::Organic;
		seed.health = 100;
		seed.alive = true;
		seed.flags = w::kEntityFlagPlayer;
		seed.engine_flags = w::kEntityFlagPlayer;
		body = world.registry.spawn(0, seed);
		w::AiEntity *ae = world.ai.at(world.ai.attach(body));
		if (!expect(ae != nullptr, "the body takes a brain")) return;
		ae->inf.active = true;
		ae->inf.anim_state = as::kIdle;
		ae->inf.wpn_state = as::kReload;
		ae->inf.wpn_playing_state = as::kReload;
		const w::Entity *e = world.registry.get(body);
		if (!expect(pose.register_skeletal_entity(body, e->registry_spawn_id, kModelId, kAdm, model),
					"the body's rig registers"))
			return;
		if (!expect(pose.register_skeletal_type(kPersonType, kModelId, kAdm, model),
					"the type's rig registers"))
			return;
		const anim::SkeletalClips *rig = pose.skeletal_rig(body);
		sections.sections.resize(rig != nullptr ? rig->bone_count() : 0);
		ok = !sections.sections.empty();
	}

	w::Entity &entity() { return *world.registry.get(body); }
	w::AiEntity &ai() { return *world.ai.for_handle(body); }

	std::vector<w::CollisionMatrix> posed() {
		std::vector<w::CollisionMatrix> out;
		expect(pose.build_section_matrices(world, body, kModelId, entity_world, sections, out),
				"the body's sections pose");
		return out;
	}
	std::vector<w::CollisionMatrix> oracle(bool weapon) {
		std::vector<w::CollisionMatrix> out;
		expect(pose.build_skeletal_from_inputs(kPersonType, kModelId,
					   inputs_for(ai(), entity(), weapon), entity_world, sections, out),
				"the oracle's sections pose");
		return out;
	}
};

// The splice leaves the composition only where the gate holds: the entity's sections are the
// channel-on composition for a remote player on foot in a 0x40 state, and the channel-off one
// without the Player bit, in a control / gunner / driver seat, or in a state without 0x40. A
// passenger keeps it; the local body takes it with no armed test.
void test_every_player_body_takes_the_splice() {
	Scene s;
	if (!s.ok) return;
	// The playhead where the reload moves the masked sections furthest off the locomotion pose.
	int32_t tick = -1;
	int64_t widest = 0;
	for (int32_t t = 0; t < 300; ++t) {
		s.ai().inf.wpn_clip_phase = t;
		const int64_t d = distance(s.oracle(true), s.oracle(false));
		if (d > widest) {
			widest = d;
			tick = t;
		}
	}
	if (!expect(tick >= 0, "the reload clip moves a masked section")) return;
	s.ai().inf.wpn_clip_phase = tick;
	expect(!s.world.cached.local_player.valid(), "the body is a remote player");

	struct Case {
		uint32_t flags;
		bool mounted;
		w::SeatType seat;
		int state;
		bool spliced;
		const char *what;
	};
	const Case cases[] = {
		{w::kEntityFlagPlayer, false, w::SeatType::None, as::kIdle, true,
				"a remote player on foot in a 0x40 state takes the splice"},
		{0u, false, w::SeatType::None, as::kIdle, false,
				"a body without the Player bit takes none"},
		{w::kEntityFlagPlayer, true, w::SeatType::Passenger, as::kIdle, true,
				"a passenger keeps the splice"},
		{w::kEntityFlagPlayer, true, w::SeatType::Controller, as::kIdle, false,
				"a controller seat blocks it"},
		{w::kEntityFlagPlayer, true, w::SeatType::Gunner, as::kIdle, false,
				"a gunner seat blocks it"},
		{w::kEntityFlagPlayer, true, w::SeatType::Driver, as::kIdle, false,
				"a driver seat blocks it"},
		{w::kEntityFlagPlayer, false, w::SeatType::None, as::kRollLeft, false,
				"a requested state without flag 0x40 blocks it"},
	};
	for (const Case &c : cases) {
		w::Entity &e = s.entity();
		e.flags = c.flags;
		e.engine_flags = c.flags;
		e.mounted = c.mounted;
		e.mount_type = c.seat;
		s.ai().inf.anim_state = c.state;
		const std::vector<w::CollisionMatrix> on = s.oracle(true);
		const std::vector<w::CollisionMatrix> off = s.oracle(false);
		expect(!same(on, off), "the channel moves this body's masked sections");
		expect(same(s.posed(), c.spliced ? on : off), c.what);
	}

	// The anchors ride the same pose: the held-weapon hand moves with the splice.
	w::Entity &e = s.entity();
	e.mounted = false;
	e.mount_type = w::SeatType::None;
	s.ai().inf.anim_state = as::kIdle;
	int32_t spliced[3] = {}, plain[3] = {};
	e.flags = e.engine_flags = w::kEntityFlagPlayer;
	const bool have_spliced = s.pose.resolve_skeletal_anchor(
			s.world, s.body, w::SkeletalAnchor::HeldWeapon, spliced);
	e.flags = e.engine_flags = 0u;
	const bool have_plain = s.pose.resolve_skeletal_anchor(
			s.world, s.body, w::SkeletalAnchor::HeldWeapon, plain);
	expect(have_spliced && have_plain &&
					std::memcmp(spliced, plain, sizeof(spliced)) != 0,
			"a remote player's held-weapon anchor takes the splice");

	// The local body: the same gate, no armed test.
	e.flags = e.engine_flags = w::kEntityFlagPlayer;
	s.world.cached.local_player = s.body;
	expect(same(s.posed(), s.oracle(true)), "the local body takes the splice, armed or not");
}

// A joiner's seated player row: the projection and the collision pose's inputs take the seat
// test, so a control / gunner / driver seat blocks the splice in a 0x40 state (sit_N) and a
// passenger seat keeps it; a player row never collapses its right-hand row.
void test_seated_player_row_takes_the_seat_test() {
	namespace rep = opennova::replication;
	namespace inm = opennova::inmatch;
	constexpr uint16_t kRow = 0x0003;
	constexpr uint16_t kCarrier = 0x1010;
	constexpr uint16_t kCarrierType = 0x0400;
	constexpr uint8_t kBone = 3;
	struct Case {
		w::SeatType seat;
		bool spliced;
		const char *what;
	};
	const Case cases[] = {
		{w::SeatType::Passenger, true, "a seated passenger row keeps the splice"},
		{w::SeatType::Controller, false, "a controller row takes none"},
		{w::SeatType::Gunner, false, "a gunner row takes none"},
		{w::SeatType::Driver, false, "a driver row takes none"},
	};
	expect((w::infantry_anim_flags(as::kSit) & 0x40u) != 0, "sit_0 carries flag 0x40");
	for (const Case &c : cases) {
		std::vector<mission::ItemSeatSpec> specs(1);
		specs[0].type_id = kCarrierType;
		w::Seat seat;
		seat.type = c.seat;
		seat.bone_index = kBone;
		specs[0].seats.push_back(seat);
		rep::ClientState state;
		{
			rep::ClientEntityState &carrier = state.upsert(kCarrier);
			carrier.type_id = kCarrierType;
			carrier.cls = EntityClass::Vehicle;
		}
		rep::ClientEntityState &row = state.upsert(kRow);
		row.cls = EntityClass::Player;
		row.type_id = kPersonType;
		row.rm_adm_id = 0;
		row.rm_state = static_cast<int16_t>(as::kSit);
		row.anim_state_id = static_cast<uint8_t>(as::kSit);
		row.carrier_handle = kCarrier;
		row.mount_bone = kBone;
		row.wpn_playing = static_cast<int16_t>(as::kReload);
		row.wpn_phase = 4;
		w::WeaponTable weapons;
		const inm::ClientReplicaPresentContext context{&specs, &weapons, true, nullptr};
		float pf[w::PF_STRIDE];
		inm::initialize_client_replica_present_row(pf);
		inm::project_client_replica_present_row(pf, row, state, context);
		w::SkeletalPoseInputs in;
		if (!expect(inm::skeletal_pose_inputs_for_client(row, state, context, in),
					"the seated row's pose inputs build"))
			continue;
		expect(in.weapon == c.spliced, c.what);
		expect((static_cast<int>(pf[w::PF_WPN_ANIM_STATE]) == as::kReload) == c.spliced,
				"the projection presents the same verdict");
		expect(!in.collapse_right_hand && pf[w::PF_RIGHT_HAND_COLLAPSED] == 0.0f,
				"a player row never collapses its right-hand row");
	}
}

} // namespace

int main() {
	test_every_player_body_takes_the_splice();
	test_seated_player_row_takes_the_seat_test();
	if (failures != 0) {
		std::fprintf(stderr, "collision_pose_splice: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("collision_pose_splice: OK\n");
	return 0;
}
