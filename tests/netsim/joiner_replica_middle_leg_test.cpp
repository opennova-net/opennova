// D-NET-196 (L5-s13 row 2): a joiner's replica pool-0 rows update in the World's entity
// pass, as retail's pool-0 walk updates every pool-0 body after the pool-1 walk, the
// projectiles and the proximity build. The rows through the own body's slot take their
// visits after the proximity build and ahead of the own body's update. Each visited
// person's collision proxy then carries its live Position and live dead bit, which the
// own body's resolver re-reads through the table's entity pointer, while the proxy's
// table copy (the reach gate's) keeps the position it was built at. A row past the own
// slot moves and dies behind the own body (L5-s14, V-ROLL-P's D1 / D2 on row 2); its
// live words reach its proxy after the World's entity pass, for the frame's weapon
// actions, and its copy stays as built (L5-s15).
// [orig: Entity_UpdateAllEntities @0x4C2100 - the proximity build @0x4C240A, the pool-0
//  walk @0x4C2426..0x4C2472; Entity_MovementCollisionResolver @0x4B2BD0 (the reach gate
//  on the copy @0x4B3AF6..0x4B3B74, the dead skip @0x4B3B89..0x4B3B8D, the live reads
//  @0x4B3B93..0x4B3B99); Entity_UpdateInfantryPlayerBody @0x4B40E0 (the death gate
//  @0x4B4BF1)]

#include <net/npwire/idatagram_socket.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_keys.h>
#include <net/protocol/protocol.h>
#include <base/gameprofile/gameprofile.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/joiner_role.h>
#include <runtime/inmatch/napi_np_connection.h> // make_jo_game_session_sequencing
#include <runtime/mission/mission_kernel.h>
#include <runtime/world/collision.h>
#include <runtime/world/entity.h>
#include <runtime/world/player_spawn.h>

#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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
const std::string kClientScrk = "CLIENT-BRIDGE-SCRK";
const std::string kServerScrk = "SERVER-BRIDGE-SCRK";
constexpr uint32_t kClientKey = 0x0000BEEFu;
constexpr uint32_t kSessionId = 0x0FE0E112u;
constexpr uint16_t kLow = 0x0003;  // a remote player below the own slot
constexpr uint16_t kSelf = 0x0005; // the own row
constexpr uint16_t kHigh = 0x0007; // a remote player past the own slot

class NullSocket final : public opennova::IDatagramSocket {
public:
	int recv_from(uint8_t *, std::size_t, PeerAddr &) override { return 0; }
	void send_to(const PeerAddr &, const uint8_t *, std::size_t) override {}
};

struct Harness {
	std::unique_ptr<mission::MissionKernel> kernel = std::make_unique<mission::MissionKernel>();
	inmatch::JoinerRole role;
	NullSocket socket;
	inmatch::TickInput input;
	SessionSequencing seq = inmatch::make_jo_game_session_sequencing();
	Harness() {
		kernel->world.registry.configure_pool(0, 16);
		kernel->wire_collision(); // the world's collision, whose person proxies the rows feed
		role.bind(*kernel);
		role.set_socket(&socket, PeerAddr{});
		role.set_protocol(&kJo);
		role.create_runtime("BridgeJoiner", inmatch::JoinRole::Player, "", "");
		role.kit_seams.apply_authoritative = [] {};
		role.kit_seams.reseed_on_side_change = [] { return false; };
		role.kit_seams.push = [] {};
		role.poll_preload();
		role.seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk, 1, 0,
				kSelf, w::kPlayerInfantryTypeId);
		role.run_tick(input);
		// The three players' rows, as their spawns left them.
		for (const uint16_t handle : {kLow, kSelf, kHigh}) {
			auto &row = role.runtime->state().upsert(handle);
			row.cls = EntityClass::Player;
			row.type_id = w::kPlayerInfantryTypeId;
		}
	}
	// One 0x0A: the three players at x units (y 2, z 0) from the frame anchor, alive, or
	// with the two remote players' records dead (Flags bit 2).
	void pump(int32_t low_x, int32_t self_x, int32_t high_x, bool remotes_dead = false) {
		FrameUpdate fu;
		fu.carried_handle = 0xFFFF;
		fu.health = 100;
		fu.anchor_x = 100 << 16;
		fu.anchor_y = 200 << 16;
		fu.anchor_z = 10 << 16;
		const std::pair<uint16_t, int32_t> players[] = {
				{kLow, low_x}, {kSelf, self_x}, {kHigh, high_x}};
		for (const auto &[handle, x] : players) {
			FrameUpdateRecord rec;
			rec.handle = handle;
			rec.type_id = w::kPlayerInfantryTypeId;
			rec.cls = EntityClass::Player;
			rec.player.carrier_handle = 0xFFFF;
			rec.player.pos_x_compressed = network_compress_fixedpoint(x << 16);
			rec.player.pos_y_compressed = network_compress_fixedpoint(2 << 16);
			rec.player.pos_z_compressed = network_compress_fixedpoint(0);
			rec.player.yaw_byte = 0x40;
			if (remotes_dead && handle != kSelf)
				rec.player.state_flags = static_cast<uint8_t>(w::kEntityFlagDead);
			fu.records.push_back(rec);
		}
		std::vector<uint8_t> packet;
		frame_session_packet(seq, SessionCrypto{kServerScrk, {}, kClientKey},
				{make_protocol_message(s2c::PER_FRAME_UPDATE, encode_frame_update(fu))}, packet);
		auto datagram = nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(packet));
		role.runtime->receive(datagram.data(), datagram.size());
		role.run_tick(input);
	}
	const replication::ClientEntityState *row(uint16_t handle) const {
		return role.runtime->state().find(handle);
	}
	const w::WirePersonCollisionProxy *proxy(uint16_t handle) const {
		return kernel->collision.wire_person_proxy(handle);
	}
};

void test_the_rows_through_the_own_slot_move_inside_the_entity_pass() {
	Harness h;
	// The rows settle at their first samples.
	for (int i = 0; i < 4; ++i) h.pump(1, 20, 40);
	const replication::ClientEntityState *low = h.row(kLow);
	const replication::ClientEntityState *high = h.row(kHigh);
	CHECK(low != nullptr && high != nullptr);
	if (low == nullptr || high == nullptr) return;
	const int32_t low_before = low->x;
	const int32_t high_before = high->x;
	CHECK(h.proxy(kLow) != nullptr && h.proxy(kHigh) != nullptr);
	CHECK(h.proxy(kSelf) == nullptr); // the own row is L's, never a proxy
	// Both remote players step 6 units: past the chase's snap distance, so each visit
	// lands its row on the sample.
	h.pump(7, 20, 46);
	low = h.row(kLow);
	high = h.row(kHigh);
	const w::WirePersonCollisionProxy *low_proxy = h.proxy(kLow);
	const w::WirePersonCollisionProxy *high_proxy = h.proxy(kHigh);
	CHECK(low != nullptr && high != nullptr && low_proxy != nullptr && high_proxy != nullptr);
	if (low == nullptr || high == nullptr || low_proxy == nullptr || high_proxy == nullptr) return;
	CHECK(low->x != low_before && high->x != high_before);
	// The low row moved inside the entity pass, ahead of the own body: its proxy took the
	// moved position as its live one, and its table copy keeps the position it was built at.
	CHECK(low_proxy->live_position_q16.x == low->x && low_proxy->live_position_q16.y == low->y &&
			low_proxy->live_position_q16.z == low->z);
	CHECK(low_proxy->position_q16.x == low_before);
	// The high row moved behind the own body: its table copy keeps the position it was
	// built at, and its live words follow it once the after-self leg ran.
	CHECK(high_proxy->live_position_q16.x == high->x);
	CHECK(high_proxy->position_q16.x == high_before);
}

// The two remote players' records go dead in one pump. The low row dies in its own update
// inside the entity pass (its death edge latches its Flags dead bit), so its proxy carries
// that live bit to the own body's resolve after it, which passes over it. The high row dies
// behind the own body, and its live bit reaches its proxy after the entity pass.
void test_a_row_dying_through_the_own_slot_carries_its_live_dead_bit() {
	Harness h;
	for (int i = 0; i < 4; ++i) h.pump(1, 20, 40);
	const w::WirePersonCollisionProxy *low_proxy = h.proxy(kLow);
	const w::WirePersonCollisionProxy *high_proxy = h.proxy(kHigh);
	CHECK(low_proxy != nullptr && high_proxy != nullptr);
	if (low_proxy == nullptr || high_proxy == nullptr) return;
	CHECK(!low_proxy->dead && !high_proxy->dead);
	h.pump(1, 20, 40, /*remotes_dead=*/true);
	const replication::ClientEntityState *low = h.row(kLow);
	const replication::ClientEntityState *high = h.row(kHigh);
	low_proxy = h.proxy(kLow);
	high_proxy = h.proxy(kHigh);
	CHECK(low != nullptr && high != nullptr && low_proxy != nullptr && high_proxy != nullptr);
	if (low == nullptr || high == nullptr || low_proxy == nullptr || high_proxy == nullptr) return;
	// Both rows' death edges ran in this tick's visits.
	CHECK((low->rm_entity_flags & w::kEntityFlagDead) != 0);
	CHECK((high->rm_entity_flags & w::kEntityFlagDead) != 0);
	CHECK(low_proxy->dead);
	CHECK(high_proxy->dead);
}

} // namespace

int main() {
	test_the_rows_through_the_own_slot_move_inside_the_entity_pass();
	test_a_row_dying_through_the_own_slot_carries_its_live_dead_bit();
	if (failures != 0) {
		std::printf("%d check(s) failed\n", failures);
		return 1;
	}
	std::printf("joiner_replica_middle_leg: ok\n");
	return 0;
}
