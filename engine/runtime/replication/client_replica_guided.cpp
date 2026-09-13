// The guided-missile guidance fold a client runs: S2C 0x44 carries a 5-B
// entity-routed sub-header [u16 shooter (entity+368)][i16 netId][u8 group]
// then ONE §5.15 field group; the non-authority client folds the discrete
// guidance moments and FLIES the missile locally between them
// [orig: NapiNPClientMsg_0x044 @0x422710 -> NetPacket_DispatchToEntityByNetId
//  @0x4D6960 -> Entity_SerializeGuidedMissileState @0x447C50 (ReadFull);
//  flight Entity_UpdateGuidedMissile_0 @0x446060 non-authority branch].
// Group semantics (fork-witnessed on the Karo reference wire — 100% stng):
//   1 (Status)        detonate/flight-end (entity+696|=1, +276|=0x1000) — the
//                     ONLY thing that ends a client-flown missile: the
//                     termination tests are authority-side [orig: the
//                     `!is_in_session || is_authority` gate @0x4463cb]
//   3/4 (TargetPos/TargetTypePos)  lock acquired: target_slot + steer point
//   2 (ClearTarget)   lock lost: clears the target [orig: read side clears
//                     entity+724/+728; authority track-loss send @0x44645a]
//   5 (Pos)           flare decoy: the flare position becomes the steer point
//                     and the entity lock is CLEARED [orig: read side clears
//                     the target; write site zeroes legChaseYawR @0x4462ec
//                     before sending group 5 @0x446324]
//   6 (AttachOffsets) no presented surface — dropped.
// RESIDUALS (D-NET-64 row): flight velocity/turn clamps use the integrator
// defaults until the missile's ammo identity resolves through the entity
// class (the ammo.def turnrate fields are parsed and carried already);
// presentation of the flown missile (model + trail) is the host's follow-up.

#include <runtime/replication/client_replica_pipeline.h>

#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_message_id.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace opennova::replication {

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
		++malformed_bodies_;
		return;
	}
	const auto group = static_cast<GuidedFieldGroup>(pkt.subtype);
	GuidedRecord rec;
	size_t used = 0;
	switch (group) {
		case GuidedFieldGroup::Status:
		case GuidedFieldGroup::ClearTarget:
		case GuidedFieldGroup::TargetPos:
		case GuidedFieldGroup::TargetTypePos:
		case GuidedFieldGroup::Pos:
			if (!decode_guided_field_group(GuidedMode::ReadFull, group,
			                                       pkt.body, pkt.body_size, rec, used))
				return;
			break;
		default:
			return;   // AttachOffsets — no presented surface
	}

	// Find or create the missile row, keyed by net id. Terminated rows are
	// reclaimable — the retail equivalent is the entity table freeing the dead
	// missile's slot, after which its net id can carry a NEW missile.
	ClientGuidedMissile *m = nullptr;
	ClientGuidedMissile *free_slot = nullptr;
	for (ClientGuidedMissile &g : state_.guided) {
		if (g.active && g.net_id == pkt.net_id) { m = &g; break; }
		if ((!g.active || g.terminated) && free_slot == nullptr) free_slot = &g;
	}
	if (m != nullptr && m->terminated && group != GuidedFieldGroup::Status) {
		// A non-Status record on a dead row = the net id was recycled for a
		// fresh missile; reset the row (repeated group-1 dead-state
		// rebroadcasts, 110 on the Karo wire, keep matching the dead row).
		*m = ClientGuidedMissile{};
		m->active = true;
		m->net_id = pkt.net_id;
		m->shooter = pkt.field0;
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
		case GuidedFieldGroup::ClearTarget:
			m->lock_target = 0xFFFF;   // lock lost; the steer point stays
			break;
		case GuidedFieldGroup::TargetPos:
		case GuidedFieldGroup::TargetTypePos:
		case GuidedFieldGroup::Pos:
			// Locks carry target + steer point; a decoy's flare position
			// becomes the steer point and CLEARS the entity lock (the seeker
			// chases the flare) [orig: the group-5 read clears the target;
			// write site @0x4462ec..0x446324; flare identity via
			// Entity_IsShellProjectile @0x4E4040].
			m->lock_target = (group == GuidedFieldGroup::Pos) ? 0xFFFF
			                                                  : rec.target_slot;
			// The read side stores every coordinate it decodes into
			// entity+700/704/708 unconditionally, zero included (a short
			// body stores 0 and flags the context); there is no all-zero
			// skip, so an origin steer point is a real steer point.
			// [orig: Entity_SerializeGuidedMissileState @0x447C50, read-full
			//  group 3 @0x447EEB/@0x447F08/@0x447F2E, group 4 @0x4480E5/
			//  @0x448102/@0x448129, group 5 @0x448151/@0x44816E/@0x448195]
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
			break;
		default:
			break;
	}
}

void ClientReplicaPipeline::tick_guided_missiles() {
	for (ClientGuidedMissile &g : state_.guided) {
		if (!g.active || g.terminated || !g.flight_seeded || !g.has_steer) continue;
		// Non-authority: fly only. The client runs NO termination tests — the
		// wire's group 1 is what ends the flight [orig: the authority gate
		// @0x4463cb; dead-bit fold above].
		world::GuidedFlight::step(g.flight, g.steer, kDefaultVelocity,
		                          /*turn_max_pit=*/0, /*turn_max_yaw=*/0,
		                          /*authority=*/false);
	}
}

} // namespace opennova::replication
