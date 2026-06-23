#include "netsim/entity_wire_bridge.h"

#include <cmath>      // std::lround

#include <world/ai.h>   // AiEntity / AiSystem (engine-frame mirror)
#include <world/geom.h> // to_fixed / from_fixed

namespace opennova::netsim {

// The player infantry item template (§5.2a host-built player entity; the same id
// PlayerReplicationState::entity_type_id defaults to).
static constexpr uint16_t kPlayerInfantryTypeId = 0x14B9u;

EntityClass class_for_type_id(uint16_t type_id) {
	// Phase 1 minimal table. Phase 3 derives this from the item's *_function class
	// tag in items.def [orig: ItemDef+356]. Every replicated non-player type is
	// treated as AI infantry for now (org0/org1 — §5.14).
	if (type_id == kPlayerInfantryTypeId) return EntityClass::Player;
	return EntityClass::Infantry;
}

EntityClass entity_class_of(const world::Entity &e) {
	// Must agree with class_for_type_id for every entity snapshot_world emits.
	if (e.item_id == kPlayerInfantryTypeId) return EntityClass::Player;
	if (e.kind == world::EntityKind::Organic) return EntityClass::Infantry;
	// Markers / items / buildings have no §5.10b compact form — not 0x0A-replicated.
	return EntityClass::Unknown;
}

GameEntitySnapshot snapshot_of(const world::Entity &e) {
	GameEntitySnapshot s;
	s.pool = static_cast<uint8_t>(e.handle.pool());
	s.slot = static_cast<uint16_t>(e.handle.slot());
	s.type_id = static_cast<uint16_t>(e.item_id);
	s.flags = 0;
	s.team = e.team;
	// World stores mission-space floats; the wire is i32 16.16 (world::to_fixed).
	s.x = world::to_fixed(e.position.x);
	s.y = world::to_fixed(e.position.y);
	s.z = world::to_fixed(e.position.z);
	// Entity+16 on the wire is a 32-bit engine-frame BAM heading = (90 - mission_yaw) *
	// kBamPerDegree (D-NET-86 / §5.23/§5.24 — the same convention promote.cpp:87 and ai.cpp
	// build from). Entity::yaw is mission yaw in DEGREES, so reconcile our two-store split
	// (Entity::yaw degrees vs AiEntity::heading BAM) at the wire boundary here: the prior
	// `yaw << 16` was both the wrong scale (deg*65536, ~182x off) and missing the (90 - yaw)
	// frame inversion, collapsing nearly every facing to ~yaw 90 -> NPCs faced the wrong way.
	// The encoder takes the rounded high byte and the present inverts (90 - bam/deg) back to
	// mission yaw, so listen-server NPCs now face the same way as the AI-pool present and the
	// local-player avatar (which bypasses this bridge). [orig: Entity_SpawnFromBMSRecord
	// @0x40e9f0; resolves open question Q1]
	constexpr int64_t kBamPerDegree = 11930464; // 2^32 / 360
	s.euler_z = static_cast<int32_t>(static_cast<int64_t>(90 - e.yaw) * kBamPerDegree);
	s.entity_class = entity_class_of(e);
	return s;
}

std::vector<GameEntitySnapshot> snapshot_world(const world::World &w) {
	std::vector<GameEntitySnapshot> out;
	w.registry.for_each([&](const world::Entity &e) {
		GameEntitySnapshot s = snapshot_of(e);
		if (s.entity_class == EntityClass::Unknown) return; // no 0x0A compact form
		out.push_back(s);
	});
	return out;
}

bool apply_player_intent(world::World &world, const PlayerIntent &intent) {
	// 1. Resolve the joiner's owned entity by its wire handle (pool<<12 | slot).
	//    [orig: dispatch_entity_packet_callback @0x4D6A80 resolves g_pool_list[h>>12] and
	//    verifies `entity == *owner_ctx` before invoking the +356 callback — the receive
	//    path has NO entity+286/entity+36 health gate; that gate is send-side only
	//    (Player_BuildTag0CInputBody @0x42A550).]
	world::Entity *ent =
			world.registry.get(world::EntityHandle{static_cast<uint16_t>(intent.entity_handle)});
	if (ent == nullptr) return false;

	// 2. Never read-apply the host's OWN player — it is motor-from-raw-input, never a
	//    self-applied pose (§5.38 / ADR-0012 amendment).
	if (world.cached.local_player.valid() && ent->handle == world.cached.local_player)
		return false;

	// 3. Movement/spawn gate: skip the apply while entity+0x24 bit1 is set (spawning).
	//    [orig: case 4 gate @0x4c2000 `test [edi+24h], 2; jnz skip`.] The host-session
	//    globals the original also gates on (g_spawn_success_gate==0, playerSlot+0x20==6
	//    in-game, dword_C8D824==0 @0x4c200a-0x4c2028) hold for an active in-game peer and
	//    are modeled implicitly here (the SP listen-server only drains C2S for joined peers).
	if ((ent->flags & 0x2u) != 0)
		return false;

	// Engine-frame conversions. Heading/pitch on the extended wire are an i16 sign-extended
	// and << 16 = a full 32-bit BAM — a PURE widen, NOT the (90 - yaw) mission framing the
	// FORWARD snapshot_of applies (the joiner serialized its live entity+0x10, already
	// engine-framed). [orig: case 4 @0x4c1da6 `movsx eax, ax; shl eax, 10h` / @0x4c1dca.]
	const int32_t heading_bam = static_cast<int32_t>(intent.heading) << 16;
	const int32_t pitch_bam = static_cast<int32_t>(intent.pitch) << 16;

	// 4. SNAP the registry Entity — the store snapshot_of reads and the S2C 0x0A frame
	//    re-broadcasts. The inverse of snapshot_of's two-store read at the wire boundary.
	//    [orig: case 4 live-pos snap @0x4c2084-0x4c208e + live orientation mirror
	//    @0x4c206a/@0x4c206d.] Extended-wire position is ABSOLUTE world (no map-origin add
	//    on receive); the mounted vehicle-local transform (vehicle_handle != 0xFFFF) is a
	//    tracked deferral [orig: Entity_TransformLocalToWorld @0x43BD00].
	ent->position.x = static_cast<float>(world::from_fixed(intent.pos_x));
	ent->position.y = static_cast<float>(world::from_fixed(intent.pos_y));
	ent->position.z = static_cast<float>(world::from_fixed(intent.pos_z));
	// BAM32 -> mission yaw degrees: yaw = 90 - bam / kBamPerDegree (the exact inverse of
	// snapshot_of's `(90 - yaw) * kBamPerDegree`), normalized into [0, 360).
	constexpr double kBamPerDegree = 11930464.0; // 2^32 / 360 (matches snapshot_of)
	const long yaw_deg = std::lround(90.0 - static_cast<double>(heading_bam) / kBamPerDegree);
	ent->yaw = static_cast<int16_t>(((yaw_deg % 360) + 360) % 360);

	// 5. Mirror the engine-frame store (AiEntity) and stage the smooth-target the CLIENT
	//    interpolation consumes; mark the entity net-snapped so the infantry motor SKIPS it
	//    (the host does not re-simulate a read-applied peer). [orig: case 4 staging +0x234/
	//    240/244 @0x4c2042-0x4c205e, live +4/+0x10/+0x14 mirror, progress +0x27C=0 @0x4c20a9;
	//    motor skip @0x4b9a03.] No AiEntity (peer not AI-attached) -> registry snap stands alone.
	if (world.ai != nullptr) {
		if (world::AiEntity *ae = world.ai->for_handle(ent->handle)) {
			ae->net_is_remote_peer = true;
			ae->pos[0] = intent.pos_x; // live +4/+8/+0xC
			ae->pos[1] = intent.pos_y;
			ae->pos[2] = intent.pos_z;
			ae->heading = heading_bam; // live +0x10
			ae->pitch = pitch_bam;     // live +0x14
			ae->net_smooth_target[0] = intent.pos_x; // +0x234
			ae->net_smooth_target[1] = intent.pos_y; // +0x238
			ae->net_smooth_target[2] = intent.pos_z; // +0x23C
			ae->net_smooth_heading = heading_bam;    // +0x240
			ae->net_smooth_pitch = pitch_bam;        // +0x244
			ae->net_interp_progress = 0;             // +0x27C reset
		}
	}
	return true;
}

} // namespace opennova::netsim
