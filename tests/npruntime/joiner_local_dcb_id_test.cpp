// D-NET-919 (L5-s16 row 1, L4-s20 FOUND): a joiner's own body carries its spawn
// record's +0x154 slot byte, and its DcbId (+0x7C) is the plyr class init's 10000 + that
// byte, whichever spawn built it. The 0x18 runs the init after its store. A 0x0C body's
// own init runs ahead of the store, but the mission start re-inits every pool-0 row
// ahead of Player_InitPlayer, where the local player is bound, so its DcbId takes the
// byte too.
// [orig: NapiNPClientMsg_0x00C @0x42E730 - Entity_InitFromItemDef @0x42E84D ahead of the
//  +0x154 store @0x42EA06; NapiNPClientMsg_FullEntitySpawn @0x433780 - the store @0x433BBB
//  ahead of the init call @0x433E82; Game_StartMission @0x524360 - Entity_InitAllFromModels
//  @0x52567F, Player_InitPlayer @0x525BBC; Entity_InitAllFromModels @0x40E460 - the pool-0
//  init callbacks @0x40E5DA..0x40E60B; PlayerClass_InitEntity @0x4B1060 - 10000 + [+0x154]
//  @0x4B1149..0x4B1155]

#include <net/npwire/idatagram_socket.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/protocol/protocol.h>
#include <base/gameprofile/gameprofile.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/joiner_role.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/world/ai.h> // entity_dcb_id
#include <runtime/world/entity.h>
#include <runtime/world/player_spawn.h>

#include <cstdio>
#include <memory>
#include <string>

namespace {

using namespace opennova;
namespace inmatch = opennova::inmatch;
namespace w = opennova::world;

int failures = 0;
#define CHECK(c)                                                                               \
	do {                                                                                       \
		if (!(c)) {                                                                            \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                           \
			++failures;                                                                        \
		}                                                                                      \
	} while (0)

const opennova::Protocol &kJo = *opennova::title_protocol("jo");
constexpr uint32_t kClientKey = 0x0000BEEFu;
constexpr uint32_t kSessionId = 0x0FE0E112u;
constexpr uint16_t kSelf = 0x0005; // the own body's wire slot
constexpr uint8_t kRosterSlot = 2; // its roster slot id, the record's +0x154 byte

class NullSocket final : public opennova::IDatagramSocket {
public:
	int recv_from(uint8_t *, std::size_t, PeerAddr &) override { return 0; }
	void send_to(const PeerAddr &, const uint8_t *, std::size_t) override {}
};

// A seeded joiner whose own record the given spawn message folds before its first frame,
// the frame that binds the local body.
struct Harness {
	std::unique_ptr<mission::MissionKernel> kernel = std::make_unique<mission::MissionKernel>();
	inmatch::JoinerRole role;
	NullSocket socket;
	inmatch::TickInput input;
	explicit Harness(uint8_t tag) {
		kernel->world.registry.configure_pool(0, 16);
		role.bind(*kernel);
		role.set_socket(&socket, PeerAddr{});
		role.set_protocol(&kJo);
		role.create_runtime("DcbJoiner", inmatch::JoinRole::Player, "", "");
		role.kit_seams.apply_authoritative = [] {};
		role.kit_seams.reseed_on_side_change = [] { return false; };
		role.kit_seams.push = [] {};
		role.poll_preload();
		role.seed_session(kSessionId, kClientKey, "CLIENT-BRIDGE-SCRK",
				"SERVER-BRIDGE-SCRK", 1, 0, kSelf, w::kPlayerInfantryTypeId);
		if (tag == s2c::ENTITY_SPAWN_BATCH) {
			OrganicSpawnRecord rec;
			rec.slot_id = kSelf;
			rec.def_type = 3;
			rec.item_type_id = w::kPlayerInfantryTypeId;
			rec.minimap_flags = static_cast<uint16_t>(w::kEntityFlagPlayer);
			rec.team = 1;
			rec.player_slot_id = kRosterSlot;
			OrganicSpawnBatch batch;
			batch.entity_count = 1;
			batch.records.push_back(rec);
			role.runtime->view().apply(tag, encode_organic_spawn_batch(batch));
		} else {
			FullEntitySpawnRecord rec;
			rec.slot_id = kSelf;
			rec.item_type_id = w::kPlayerInfantryTypeId;
			rec.item_type = 3;
			rec.team = 1;
			rec.minimap_flags = static_cast<uint16_t>(w::kEntityFlagPlayer);
			rec.player_slot_id = kRosterSlot;
			role.runtime->view().apply(tag, encode_full_entity_spawn(rec));
		}
		role.run_tick(input);
	}
};

void check_the_local_body_takes_the_records_slot(uint8_t tag) {
	auto h = std::make_unique<Harness>(tag);
	const replication::ClientEntityState *row = h->role.runtime->state().find(kSelf);
	CHECK(row != nullptr && row->spawn_byte_154 == kRosterSlot);
	const w::Entity *local = h->kernel->local.player();
	CHECK(local != nullptr);
	if (local == nullptr) return;
	CHECK(local->player_slot_id == kRosterSlot);
	CHECK(w::entity_dcb_id(h->kernel->world, *local) == 10000 + kRosterSlot);
}

// D-NET-892 A2 (L5-s21): a 0x18 naming the own handle once L exists rebuilds L in place: the
// record's slot byte lands on the same body, whose class init runs again (its DcbId 10000 + the
// new byte). A 0x18 that built the self row ahead of L's spawn (the Harness's own
// case above) is not a rebuild.
// [orig: NapiNPClientMsg_FullEntitySpawn @0x433780 - Position @0x433B77, +0x154 @0x433BBB, the
//  class init @0x433E82]
void check_a_later_0x18_rebuilds_the_local_body() {
	auto h = std::make_unique<Harness>(s2c::ENTITY_SPAWN_BATCH);
	const w::EntityHandle body = h->kernel->world.cached.local_player;
	CHECK(body.valid());
	FullEntitySpawnRecord rec;
	rec.slot_id = kSelf;
	rec.item_type_id = w::kPlayerInfantryTypeId;
	rec.item_type = 3;
	rec.team = 1;
	rec.minimap_flags = static_cast<uint16_t>(w::kEntityFlagPlayer);
	rec.player_slot_id = 4;
	h->role.runtime->view().apply(s2c::FULL_ENTITY_SPAWN, encode_full_entity_spawn(rec));
	h->role.run_tick(h->input);
	const w::Entity *local = h->kernel->local.player();
	CHECK(local != nullptr && h->kernel->world.cached.local_player == body);
	if (local == nullptr) return;
	CHECK(local->player_slot_id == 4);
	CHECK(w::entity_dcb_id(h->kernel->world, *local) == 10000 + 4);
}

// D-NET-1243 (L5-s31 FOUND 1): the rebuild runs the plyr class init's word run, not its
// animation half alone: the local camera shake and lock tone 0, the ammo-def word 3 (the C2S
// 0x06 off32 byte), the equipped byte the list's first weapon (the C2S 0x0C byte 24), the arms
// dip and the reload window 0, the legs and the body heading the record's Yaw, which is the look
// yaw too; the Pitch, the look pitch and savedLivePitch 0.
// [orig: NapiNPClientMsg_FullEntitySpawn @0x433780 - Yaw @0x433B8C, the class init called
//  @0x433E82; PlayerClass_InitEntity @0x4B1060 - @0x4B10D0..0x4B10DE, @0x4B1105, @0x4B1116,
//  @0x4B117C..0x4B11CD]
void check_a_later_0x18_runs_the_class_inits_word_run() {
	auto h = std::make_unique<Harness>(s2c::ENTITY_SPAWN_BATCH);
	w::Entity *before = h->kernel->local.player();
	w::AiEntity *ai = h->kernel->local.player_ai();
	CHECK(before != nullptr && ai != nullptr);
	if (before == nullptr || ai == nullptr) return;
	before->ammo_def_word = 0x13;
	before->equipped_adm_index = 9;
	ai->inf.body_heading = 0x10000000;
	ai->inf.leg_yaw[0] = ai->inf.leg_yaw[1] = 0x12340000;
	ai->inf.leg_target[0] = ai->inf.leg_target[1] = 0x13570000;
	ai->inf.arms_dip_ticks = 9;
	ai->inf.reload_anim_ticks = 80;
	h->kernel->local.view.shake.counter = 40;
	h->kernel->local.radar.lock_tone = 31;
	FullEntitySpawnRecord rec;
	rec.slot_id = kSelf;
	rec.item_type_id = w::kPlayerInfantryTypeId;
	rec.item_type = 3;
	rec.team = 1;
	rec.minimap_flags = static_cast<uint16_t>(w::kEntityFlagPlayer);
	rec.player_slot_id = kRosterSlot;
	rec.heading_hi = 0x2000;
	rec.pitch_hi = 0x0100;
	h->role.runtime->view().apply(s2c::FULL_ENTITY_SPAWN, encode_full_entity_spawn(rec));
	h->role.run_tick(h->input);
	const w::Entity *local = h->kernel->local.player();
	ai = h->kernel->local.player_ai();
	CHECK(local != nullptr && ai != nullptr);
	if (local == nullptr || ai == nullptr) return;
	static constexpr int32_t kYaw = 0x20000000;
	CHECK(local->ammo_def_word == 3);
	CHECK(local->equipped_adm_index == w::player_spawn_equipped_default(h->kernel->world));
	CHECK(ai->inf.body_heading == kYaw);
	CHECK(ai->inf.leg_yaw[0] == kYaw && ai->inf.leg_yaw[1] == kYaw);
	CHECK(ai->inf.leg_target[0] == kYaw && ai->inf.leg_target[1] == kYaw);
	CHECK(ai->inf.target_heading == kYaw);
	CHECK(ai->inf.arms_dip_ticks == 0 && ai->inf.reload_anim_ticks == 0);
	CHECK(local->pitch_bam == 0 && ai->inf.look_pitch == 0 && local->saved_live_pitch == 0);
	CHECK(h->kernel->local.view.shake.counter == 0);
	CHECK(h->kernel->local.radar.lock_tone == 0);
}

} // namespace

int main() {
	check_the_local_body_takes_the_records_slot(s2c::ENTITY_SPAWN_BATCH);
	check_the_local_body_takes_the_records_slot(s2c::FULL_ENTITY_SPAWN);
	check_a_later_0x18_rebuilds_the_local_body();
	check_a_later_0x18_runs_the_class_inits_word_run();
	if (failures != 0) {
		std::printf("%d check(s) failed\n", failures);
		return 1;
	}
	std::printf("npruntime_joiner_local_dcb_id: ok\n");
	return 0;
}
