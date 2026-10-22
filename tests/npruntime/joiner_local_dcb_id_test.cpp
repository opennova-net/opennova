// D-NET-NEW-L5-18 (L5-s16 row 1, L4-s20 FOUND): a joiner's own body carries its spawn
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
#include <net/title/title_identity.h>
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

const opennova::TitleIdentity &kJo = *opennova::title_identity(opennova::gameprofile::GAME_JO);
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
		role.set_title(&kJo);
		role.create_runtime("DcbJoiner", inmatch::JoinRole::Player, "", "");
		role.kit_seams.apply_authoritative = [] {};
		role.kit_seams.reseed_on_side_change = [] { return false; };
		role.kit_seams.push = [] {};
		role.poll_preload();
		role.runtime->seed_session(kSessionId, kClientKey, "CLIENT-BRIDGE-SCRK",
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

} // namespace

int main() {
	check_the_local_body_takes_the_records_slot(s2c::ENTITY_SPAWN_BATCH);
	check_the_local_body_takes_the_records_slot(s2c::FULL_ENTITY_SPAWN);
	if (failures != 0) {
		std::printf("%d check(s) failed\n", failures);
		return 1;
	}
	std::printf("npruntime_joiner_local_dcb_id: ok\n");
	return 0;
}
