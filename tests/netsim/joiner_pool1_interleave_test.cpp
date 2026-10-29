// D-NET-196 (L5-s16, design (c)): a joiner's decoded pool-1 rows take their visits at their
// own slots of the World's pool-1 walk, interleaved with the World's movers in slot order,
// as retail's walk updates every pool-1 slot in slot order (its ground chain first) and each
// visit runs the row's +0x1C4 mover. A World mover's pose reaches its row, and its proxy's
// live words, right after its mover at its slot, so the projectile pass after the walk reads
// this tick's pose; a hull riding a deck the walk moved at a lower slot follows it as it
// stands this tick.
// [orig: Entity_UpdateAllEntities @0x4C2100 - the pool-1 walk @0x4C2158..0x4C21E9 (the
//  Entity_UpdatePool1Slot calls @0x4C21B9 / @0x4C21D0 / @0x4C21E0 / @0x4C21E9), the
//  Weapon_UpdateAllProjectiles call @0x4C223A, the proximity build @0x4C240A;
//  Entity_UpdatePool1Slot @0x4B8DD0 - the +0x1C4 mover @0x4B8E41..0x4B8E53;
//  Projectile_RaycastProximitySlots @0x4E5340 (g_DynProxEntity @0x4E5760)]

#include <formats/def/def.h>
#include <formats/mission/mission.h> // kItemIdOffset
#include <base/gameprofile/gameprofile.h>
#include <net/protocol/protocol.h>
#include <net/npwire/entity_class.h>
#include <net/npwire/idatagram_socket.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/peer_addr.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/join_role.h>
#include <runtime/inmatch/joiner_role.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/replication/client_state.h>
#include <runtime/world/collision.h>
#include <runtime/world/entity.h>
#include <runtime/world/geom.h> // to_fixed
#include <runtime/world/player_spawn.h>
#include <runtime/world/vehicle_motor.h>
#include <runtime/world/world.h>

#include "../common/hull_brain.h"
#include "../common/replica_twin.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include "../common/test_expect.h"

namespace {

using namespace opennova;
namespace inmatch = opennova::inmatch;
namespace w = opennova::world;

const opennova::Protocol &kJo = *opennova::title_protocol("jo");

int &failures = test_expect::failures;
using test_expect::expect;

const std::string kClientScrk = "CLIENT-BRIDGE-SCRK";
const std::string kServerScrk = "SERVER-BRIDGE-SCRK";
constexpr uint32_t kClientKey = 0x0000BEEFu;
constexpr uint32_t kSessionId = 0x0FE0E112u;
constexpr uint16_t kSelfHandle = 0x0005;
constexpr uint16_t kCarrier = 0x1001; // a predicted tank: the World's mover owns it
constexpr uint16_t kDeckHull = 0x1002; // a traitless hull on its deck: the row's chase owns it
constexpr int32_t kTankType = 1296;
constexpr int32_t kHullType = 1297;
constexpr int32_t kQ = 65536;

// Both hulls' item rows: a cveh vehicle def naming no graphic, whose decoded row takes a
// model-less, candidate-eligible dynamic proxy.
def::DefItemDef g_defs[3]{};
def::DefItemsFile g_items{g_defs, 3};

class SilentSocket final : public opennova::IDatagramSocket {
public:
	int recv_from(uint8_t *, std::size_t, PeerAddr &) override { return 0; }
	void send_to(const PeerAddr &, const uint8_t *, std::size_t) override {}
};

// The kernel lives on the heap: a World-carrying frame is megabytes.
struct Harness {
	std::unique_ptr<mission::MissionKernel> kernel = std::make_unique<mission::MissionKernel>();
	inmatch::JoinerRole role;
	SilentSocket socket;
	inmatch::TickInput input;

	Harness() {
		kernel->world.registry.configure_pool(0, 16);
		kernel->world.registry.configure_pool(1, 16);
		role.bind(*kernel);
		kernel->world.load_systems();
		role.set_socket(&socket, PeerAddr{});
		role.set_title(&kJo);
		role.create_runtime("InterleaveJoiner", inmatch::JoinRole::Player, "", "");
		role.kit_seams.apply_authoritative = [] {};
		role.kit_seams.reseed_on_side_change = [] { return false; };
		role.kit_seams.push = [] {};
		role.poll_preload();
		role.seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk, 1, 0,
		                           kSelfHandle, w::kPlayerInfantryTypeId);
		role.run_tick(input);

		std::memset(g_defs, 0, sizeof(g_defs));
		g_defs[0].id = w::kPlayerInfantryTypeId + mission::kItemIdOffset;
		g_defs[0].type = 3;
		g_defs[0].hp = 100;
		for (int i = 1; i < 3; ++i) {
			g_defs[i].id = (i == 1 ? kTankType : kHullType) + mission::kItemIdOffset;
			g_defs[i].type = 1;
			std::strcpy(g_defs[i].ai_function, "cveh");
		}
		kernel->set_items_table(&g_items);
		kernel->resolve_collision_instances();

		w::VehicleTraits traits;
		traits.family = w::VehicleFamily::Tank;
		traits.physics = 1;
		traits.player_control = true;
		traits.player_speed = 94 * 293;
		kernel->world.vehicles.traits.set(kTankType, traits);
		const w::Vec3 at{60.0f, 60.0f, 0.0f};
		w::Entity tank;
		tank.kind = w::EntityKind::Item;
		tank.item_id = kTankType;
		tank.has_item_def = true;
		tank.item_type_index = 1;
		tank.item_type = 1;
		tank.net_class_code = static_cast<uint8_t>(EntityClass::Vehicle);
		tank.spawn_origin = (1u << 24) | 3u;
		tank.position = at;
		tank.health = tank.health_max = 3000;
		expect(test_common::materialize_twin_at(role.materializer(), role.runtime->state(),
		               kernel->world, w::EntityHandle{kCarrier}, tank, EntityClass::Vehicle) ==
		               w::EntityHandle{kCarrier},
		       "the tank spawns at its slot");
		test_common::brain_hull(kernel->world, w::EntityHandle{kCarrier});
		w::Entity hull = tank;
		hull.item_id = kHullType;
		hull.item_type_index = 2;
		hull.spawn_origin = (1u << 24) | 4u;
		hull.position = {at.x, at.y, at.z + 3.0f};
		expect(test_common::materialize_twin_at(role.materializer(), role.runtime->state(),
		               kernel->world, w::EntityHandle{kDeckHull}, hull, EntityClass::Vehicle) ==
		               w::EntityHandle{kDeckHull},
		       "the deck hull spawns at its slot");
		role.runtime->view().set_item_class_resolver(
				[](uint16_t type) -> std::optional<EntityClass> {
					if (type == kTankType || type == kHullType) return EntityClass::Vehicle;
					if (type == w::kPlayerInfantryTypeId) return EntityClass::Player;
					return std::nullopt;
				});
		replication::ClientEntityState &carrier = role.runtime->state().upsert(kCarrier);
		carrier.type_id = static_cast<uint16_t>(kTankType);
		carrier.cls = EntityClass::Vehicle;
		carrier.x = w::to_fixed(at.x);
		carrier.y = w::to_fixed(at.y);
		carrier.z = w::to_fixed(at.z);
		// The deck hull's compact carried it on the tank, 3 u above its root.
		replication::ClientEntityState &deck = role.runtime->state().upsert(kDeckHull);
		deck.type_id = static_cast<uint16_t>(kHullType);
		deck.cls = EntityClass::Vehicle;
		deck.x = carrier.x;
		deck.y = carrier.y;
		deck.z = carrier.z + 3 * kQ;
		deck.carrier_handle = kCarrier;
		deck.net_seat_valid = true;
		deck.net_seat_local[2] = 3 * kQ;
		deck.net_has_compact = true;
		role.run_tick(input); // the tank's row takes the World mover's flag
	}
	~Harness() { kernel->set_items_table(nullptr); }
	const replication::ClientEntityState &row(uint16_t handle) {
		return *role.runtime->state().find(handle);
	}
	const w::WireDynamicCollisionProxy *proxy(uint16_t handle) const {
		return kernel->collision.wire_dynamic_proxy(handle);
	}
	// One live compact for the tank, `dx` units along +x from where it stands, then the
	// joiner frame: its first arms the client mover, which chases the sample over ticks.
	void record_tank_at(int32_t dx) {
		const w::Entity &e = *kernel->world.registry.get(w::EntityHandle{kCarrier});
		FrameUpdate fu;
		fu.flags2 = 0;
		fu.carried_handle = 0xFFFF;
		fu.health = 100;
		fu.anchor_x = w::to_fixed(e.position.x);
		fu.anchor_y = w::to_fixed(e.position.y);
		fu.anchor_z = w::to_fixed(e.position.z);
		FrameUpdateRecord r;
		r.handle = kCarrier;
		r.type_id = static_cast<uint16_t>(kTankType);
		r.cls = EntityClass::Vehicle;
		r.vehicle.parent_slot_handle = 0xFFFF;
		r.vehicle.pos_x_compressed = network_compress_fixedpoint(dx * kQ);
		r.vehicle.pos_y_compressed = network_compress_fixedpoint(0);
		r.vehicle.pos_z_compressed = network_compress_fixedpoint(0);
		r.vehicle.health_word = 3000;
		fu.records.push_back(r);
		role.runtime->view().apply(s2c::PER_FRAME_UPDATE, encode_frame_update(fu));
		role.run_tick(input);
	}
};

// A predicted carrier at slot 1 and the traitless hull on its deck at slot 2: the carrier's
// mover moves it at its slot and its row takes the pose there, so the deck hull's own
// visit at the next slot follows the carrier as it stands this tick. The proximity build
// after the walk copies the deck hull there; before the interleave the hull followed the
// carrier's previous pose in ClientRuntime's leg, ahead of the World's walk, and the build
// copied that.
bool run_a_deck_hull_follows_its_carrier_moved_at_the_lower_slot() {
	auto h = std::make_unique<Harness>();
	bool ok = true;
	h->record_tank_at(1);
	const int32_t carrier_before = h->row(kCarrier).x;
	const int32_t deck_before = h->row(kDeckHull).x;
	ok &= expect(h->kernel->world.registry.get(w::EntityHandle{kCarrier})->veh.net_predicted,
	             "the compact armed the carrier's client mover");
	h->role.run_tick(h->input);
	const replication::ClientEntityState &carrier = h->row(kCarrier);
	const replication::ClientEntityState &deck = h->row(kDeckHull);
	ok &= expect(carrier.x != carrier_before, "the carrier's mover moved it this tick");
	const w::WireDynamicCollisionProxy *copy = h->proxy(kDeckHull);
	ok &= expect(copy != nullptr, "the deck hull has a dynamic proxy");
	if (copy == nullptr) return ok;
	ok &= expect(copy->position_q16.x == deck.x && copy->position_q16.y == deck.y &&
	                     copy->position_q16.z == deck.z,
	             "the build copied the deck hull on its carrier as the carrier stands this tick");
	ok &= expect(copy->position_q16.x != deck_before,
	             "not on the carrier's previous pose");
	return ok;
}

// The leg at a World mover's slot: its row takes the mover's pose and its proxy the live
// words, the table's copy staying the build's; a second call at the same slot this tick
// visits nothing. A pool-1 row the World's walk never reached visits at the head of the
// pool-0 halves, ahead of the parked pool-0 rows.
bool run_a_world_movers_leg_writes_its_live_words_once() {
	auto h = std::make_unique<Harness>();
	bool ok = true;
	h->record_tank_at(1);
	h->role.run_tick(h->input);
	replication::ClientReplicaPipeline &view = h->role.runtime->view();
	view.tick_remote_motion(kSelfHandle);
	ok &= expect(h->row(kCarrier).rm_parked && h->row(kDeckHull).rm_parked,
	             "the walk parks every pool-1 row for its slot");
	const w::WireDynamicCollisionProxy *proxy = h->proxy(kCarrier);
	ok &= expect(proxy != nullptr, "the carrier has a dynamic proxy");
	if (proxy == nullptr) return ok;
	const int32_t copy_x = proxy->position_q16.x;
	const int32_t carrier_before = h->row(kCarrier).x;
	const int32_t deck_before = h->row(kDeckHull).x;
	w::Entity &twin = *h->kernel->world.registry.get(w::EntityHandle{kCarrier});
	twin.position.x.q16 += 2 * kQ; // where its mover left it at its slot
	const int32_t moved_x = twin.position.x.q16;
	h->role.visit_replica_pool1_slot(w::EntityHandle{kCarrier});
	proxy = h->proxy(kCarrier);
	ok &= expect(h->row(kCarrier).x == moved_x && !h->row(kCarrier).rm_parked,
	             "the leg mirrors the mover's pose onto its row");
	ok &= expect(proxy != nullptr && proxy->live_position_q16.x == moved_x &&
	                     proxy->position_q16.x == copy_x,
	             "the proxy's live words are this tick's pose, its copy the build's");
	twin.position.x.q16 += kQ;
	h->role.visit_replica_pool1_slot(w::EntityHandle{kCarrier});
	ok &= expect(h->row(kCarrier).x == moved_x, "a second call this tick visits nothing");
	view.tick_remote_motion_through_self();
	ok &= expect(!h->row(kDeckHull).rm_parked, "the unreached deck hull took its visit");
	ok &= expect(h->row(kDeckHull).x - deck_before == moved_x - carrier_before,
	             "it followed its carrier as the leg left it");
	view.tick_remote_motion_after_self();
	return ok;
}

} // namespace

int main() {
	bool ok = true;
	ok &= run_a_deck_hull_follows_its_carrier_moved_at_the_lower_slot();
	ok &= run_a_world_movers_leg_writes_its_live_words_once();
	if (!ok || failures != 0) {
		std::fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	std::printf("netsim_joiner_pool1_interleave: ok\n");
	return 0;
}
