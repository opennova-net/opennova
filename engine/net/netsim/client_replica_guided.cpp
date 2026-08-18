// The guided-missile guidance fold a client runs: S2C 0x44 carries a 5-B
// entity-routed sub-header [u16 shooter (entity+368)][i16 netId][u8 group]
// then ONE §5.15 field group; the non-authority client folds the discrete
// guidance moments and FLIES the missile locally between them
// [orig: NapiNPClientMsg_0x044 @0x422710 -> NetPacket_DispatchToEntityByNetId
//  @0x4D6960 -> Entity_SerializeGuidedMissileState @0x447C50 (ReadFull);
//  flight Entity_UpdateGuidedMissile_0 @0x446060 non-authority branch].
// Group semantics (fork-witnessed, Karo reference wire — 100% stng):
//   1 (Status)        detonate/terminate (entity+696|=1, +276|=0x1000)
//   3/4 (TargetPos/TargetTypePos)  lock acquired: target_slot + steer point
//   5 (Pos)           flare decoy: the flare position becomes the steer point
//   2/6 (ClearTarget/AttachOffsets) no presented surface — dropped.
// RESIDUALS (D-NET-64 row): flight velocity/turn clamps use the integrator
// defaults until the missile's ammo identity resolves through the entity
// class (the ammo.def turnrate fields are parsed and carried already);
// presentation of the flown missile (model + trail) is the host's follow-up.

#include "netsim/client_replica_pipeline.h"

#include <npwire/ingame_decode.h>
#include <npwire/ingame_message_id.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace opennova::netsim {

namespace {

// The fork-witnessed default when no ammo identity is resolved: the stng
// velocity (ammo.def `velocity 300`) — recorded stand-in, not an invention:
// the reference wire is 100% stng.
constexpr int32_t kDefaultVelocity = 300;
// The launch lift: ~shoulder height so the missile clears the launcher
// (fork-witnessed launch pose; 1.2 units, 16.16).
constexpr int32_t kLaunchLiftQ16 = 78643;

} // namespace

void ClientReplicaPipeline::apply_entity_routed(const std::vector<uint8_t> &body) {
	EntityRoutedPacket pkt;
	if (!decode_entity_routed_packet(body.data(), body.size(), pkt)) {
		++unknown_tags_;
		return;
	}
	const auto group = static_cast<GuidedFieldGroup>(pkt.subtype);
	GuidedRecord rec;
	size_t used = 0;
	switch (group) {
		case GuidedFieldGroup::Status:
		case GuidedFieldGroup::TargetPos:
		case GuidedFieldGroup::TargetTypePos:
		case GuidedFieldGroup::Pos:
			if (!decode_guided_field_group(GuidedMode::ReadFull, group,
			                                       pkt.body, pkt.body_size, rec, used))
				return;
			break;
		default:
			return;   // ClearTarget / AttachOffsets — no presented surface
	}

	// Find or create the missile row, keyed by net id.
	ClientGuidedMissile *m = nullptr;
	ClientGuidedMissile *free_slot = nullptr;
	for (ClientGuidedMissile &g : state_.guided) {
		if (g.active && g.net_id == pkt.net_id) { m = &g; break; }
		if (!g.active && free_slot == nullptr) free_slot = &g;
	}
	if (m == nullptr) {
		if (free_slot == nullptr) return;   // bank full — oldest keeps flying
		m = free_slot;
		*m = ClientGuidedMissile{};
		m->active = true;
		m->net_id = pkt.net_id;
		m->shooter = pkt.field0;   // §5.36 field0 = the shooter handle
		                           // (fork witness; the doc row's "not read by
		                           // the dispatcher" gloss predates it)
	}
	m->revision++;

	switch (group) {
		case GuidedFieldGroup::Status:
			m->terminated = true;   // the flight-END marker
			break;
		case GuidedFieldGroup::TargetPos:
		case GuidedFieldGroup::TargetTypePos:
			m->lock_target = rec.target_slot;
			[[fallthrough]];
		case GuidedFieldGroup::Pos:
			// Locks carry the steer point; a decoy's flare position becomes
			// the next steer point (the seeker chases the flare) WITHOUT
			// touching lock_target [orig: @0x446324; Entity_IsShellProjectile
			// @0x4E4040].
			if (rec.pos_x || rec.pos_y || rec.pos_z) {
				m->steer[0] = rec.pos_x;
				m->steer[1] = rec.pos_y;
				m->steer[2] = rec.pos_z;
				m->has_steer = true;
				if (!m->flight_seeded) {
					// Launch pose: the shooter's folded position, lifted to
					// shoulder height, aimed at the first steer point.
					const ClientEntityState *sh = state_.find(m->shooter);
					if (sh != nullptr) {
						m->flight.pos[0] = sh->x;
						m->flight.pos[1] = sh->y;
						m->flight.pos[2] = sh->z + kLaunchLiftQ16;
					} else {
						m->flight.pos[0] = rec.pos_x;
						m->flight.pos[1] = rec.pos_y;
						m->flight.pos[2] = rec.pos_z;
					}
					world::GuidedFlight::aim_at(m->flight, m->steer);
					m->flight_seeded = true;
				}
			}
			break;
		default:
			break;
	}
}

void ClientReplicaPipeline::tick_guided_missiles() {
	for (ClientGuidedMissile &g : state_.guided) {
		if (!g.active || g.terminated || !g.flight_seeded || !g.has_steer) continue;
		const world::GuidedDetonate det = world::GuidedFlight::step(
				g.flight, g.steer, kDefaultVelocity, /*turn_max_pit=*/0,
				/*turn_max_yaw=*/0);
		if (det != world::GuidedDetonate::kNone) {
			g.terminated = true;   // the integrator's own overshoot/guard/proximity
			g.revision++;
		}
	}
}

} // namespace opennova::netsim
