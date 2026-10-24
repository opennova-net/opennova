// D-NET-196: a joiner's decoded player rows run the org2 body pass's ride link,
// the twin of world::infantry_ride_link: the occupant (+0x170) of the entity the
// row stands on (+0x28), else of that entity's own ground entity, when it is
// another body of the row's team, becomes the row's link and climbs the +0x174
// hold by 4 while under 0xF0; a tick without one spends a hold tick, and the
// link clears once the hold is spent. A guided round's lock note reads the
// link on its target (radar_note_guided_missile's first arm).
// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - @0x4B5EA9..0x4B5F2C;
//  Entity_UpdateGuidedMissile_0 @0x446060 - @0x4465DB..0x4465F5]
//
// The rows are seated (a carrier handle), so the pass runs the link without the
// chase legs: the seat's ground link is the record's store [orig: @0x4C1353].

#include <runtime/replication/client_replica_pipeline.h>

#include <net/npwire/entity_class.h>

#include <cstdint>
#include <cstdio>

namespace {

namespace ns = opennova::replication;
using opennova::EntityClass;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

constexpr uint16_t kRider = 0x0011;   // the remote person on the hull
constexpr uint16_t kDriver = 0x0012;  // the hull's occupant
constexpr uint16_t kEnemy = 0x0013;   // another team's body
constexpr uint16_t kHull = 0x1004;    // the ground entity
constexpr uint16_t kDeck = 0x1005;    // the hull's own ground entity
constexpr uint16_t kNone = 0xFFFF;

struct Ground {
	uint16_t occupant = kNone;
	uint16_t ground = kNone;
};

ns::ClientEntityState &person(ns::ClientReplicaPipeline &view, uint16_t handle, uint8_t team) {
	ns::ClientEntityState &row = view.state().upsert(handle);
	row.cls = EntityClass::Player;
	row.team = team;
	row.team_known = true;
	return row;
}

bool run_the_link_holds_and_spends() {
	ns::ClientReplicaPipeline view;
	view.set_remote_motion_mode(true);
	Ground hull;
	Ground deck;
	view.set_ground_occupant_provider([&](uint16_t ground, uint16_t &occupant, uint16_t &next) {
		const Ground *g = ground == kHull ? &hull : ground == kDeck ? &deck : nullptr;
		if (g == nullptr) return false;
		occupant = g->occupant;
		next = g->ground;
		return true;
	});
	person(view, kDriver, 1);
	person(view, kEnemy, 2);
	ns::ClientEntityState &rider = person(view, kRider, 1);
	rider.net_has_compact = true;
	rider.carrier_handle = kHull; // seated: the pass runs the link only
	rider.resolved_ground = kHull;
	hull.occupant = kDriver;
	view.tick_remote_motion(/*self_handle=*/kNone);
	const ns::ClientEntityState *r = view.state().find(kRider);
	if (!expect(r->ride_occupant == kDriver && r->ride_link_hold == 4,
	            "the ground's same-team occupant links and holds 4")) return false;
	for (int t = 0; t < 100; ++t) view.tick_remote_motion(kNone);
	r = view.state().find(kRider);
	// 0xF0 / 4 = 60 finding ticks reach the cap; the add runs only below it.
	if (!expect(r->ride_link_hold == 0xF0, "the hold climbs by 4 to 0xF0 and stops")) return false;
	// Another team's occupant is no link: the hold spends one per tick and the
	// link keeps its occupant until the hold is spent.
	hull.occupant = kEnemy;
	view.tick_remote_motion(kNone);
	r = view.state().find(kRider);
	if (!expect(r->ride_occupant == kDriver && r->ride_link_hold == 0xEF,
	            "a tick without a link spends one hold tick and keeps the occupant")) return false;
	for (int t = 0; t < 0xEF; ++t) view.tick_remote_motion(kNone);
	r = view.state().find(kRider);
	if (!expect(r->ride_occupant == kDriver && r->ride_link_hold == 0,
	            "the occupant stays while the hold runs out")) return false;
	view.tick_remote_motion(kNone);
	r = view.state().find(kRider);
	return expect(r->ride_occupant == kNone, "the spent hold clears the link");
}

bool run_the_grounds_ground_and_self_exclusions() {
	ns::ClientReplicaPipeline view;
	view.set_remote_motion_mode(true);
	Ground hull;
	Ground deck;
	view.set_ground_occupant_provider([&](uint16_t ground, uint16_t &occupant, uint16_t &next) {
		const Ground *g = ground == kHull ? &hull : ground == kDeck ? &deck : nullptr;
		if (g == nullptr) return false;
		occupant = g->occupant;
		next = g->ground;
		return true;
	});
	person(view, kDriver, 1);
	ns::ClientEntityState &rider = person(view, kRider, 1);
	rider.net_has_compact = true;
	rider.carrier_handle = kHull;
	rider.resolved_ground = kHull;
	// The hull's occupant is the rider itself: no link there; the hull's own
	// ground entity's occupant is the next candidate.
	hull.occupant = kRider;
	hull.ground = kDeck;
	deck.occupant = kDriver;
	view.tick_remote_motion(kNone);
	const ns::ClientEntityState *r = view.state().find(kRider);
	if (!expect(r->ride_occupant == kDriver && r->ride_link_hold == 4,
	            "the ground's ground's occupant links when the ground's does not")) return false;
	// The client's own row runs the World's link, not this one.
	ns::ClientReplicaPipeline own;
	own.set_remote_motion_mode(true);
	own.set_ground_occupant_provider([&](uint16_t ground, uint16_t &occupant, uint16_t &next) {
		occupant = kDriver;
		next = kNone;
		return ground == kHull;
	});
	person(own, kDriver, 1);
	ns::ClientEntityState &self = person(own, kRider, 1);
	self.net_has_compact = true;
	self.carrier_handle = kHull;
	self.resolved_ground = kHull;
	own.tick_remote_motion(/*self_handle=*/kRider);
	return expect(own.state().find(kRider)->ride_occupant == kNone,
	              "the client's own row is left to its World body");
}

} // namespace

int main() {
	bool ok = true;
	ok &= run_the_link_holds_and_spends();
	ok &= run_the_grounds_ground_and_self_exclusions();
	if (ok) std::printf("client_replica_pipeline_ride_link: OK\n");
	return ok ? 0 : 1;
}
