// A no-callback mount must never follow its own occupant (the 00TRg climbing
// .50cal repro, retail-host -> OpenNova-joiner, live-witnessed 2026-08-04).
//
// The retail S2C 0x0D page for an OCCUPIED "50cal on 180 tripod" (slot 0x100a,
// spawnFlags 0x0127 — bit 0x0100 set) carries parentHandle 0x002b = its own
// GUNNER's pool-0 handle: the entity+40 occupant/driver back-reference [orig:
// NapiNPClientMsg_0x00D @0x432c40, store @0x433289], not a transform parent.
// The gunner's own record simultaneously carries parent=0x100a (the mount).
// Folding BOTH as transform relations closes a mutual composition loop —
// refresh_carried_entities seat-composes the gunner from the gun, then
// persistent-parent recomposes the gun from the gunner, eight depths per tick
// at 62.5 Hz — and any transform-pair asymmetry ratchets the pair through the
// world (live: +1.13 u/s climb with a sweeping heading; the reported
// spinning/floating emplacement NPCs).
//
// This test rebuilds that exact decoded state with the live-witnessed poses
// and asserts both rows HOLD across the mover loop: the gun keeps its 0x0D
// spawn pose bit-for-bit, and the gunner keeps riding the (static) gun.

#include <runtime/replication/client_replica_pipeline.h>

#include <net/npwire/entity_class.h>
#include <net/npwire/ingame_decode.h>

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace {

namespace ns = opennova::netsim;
using opennova::EntityClass;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

constexpr uint16_t kGunHandle = 0x100A;    // pool 1 slot 10 (the tripod)
constexpr uint16_t kGunnerHandle = 0x002B; // pool 0 slot 43 (its gunner)
constexpr uint16_t kGunType = 0x076E;      // "50cal on 180 tripod" (ewep)
constexpr uint16_t kGunnerType = 0x0826;   // "Indonesian Soldier #11 Male"

int32_t fixed_of(double v) { return static_cast<int32_t>(v * 65536.0); }

} // namespace

int main() {
	ns::ClientReplicaPipeline view;
	view.set_remote_motion_mode(true);
	view.set_item_class_resolver([](uint16_t type)
			-> ns::ClientReplicaPipeline::ItemClassResolution {
		if (type == kGunType) return EntityClass::NoNetworkCallback;
		if (type == kGunnerType) return EntityClass::Infantry;
		return std::nullopt;
	});

	// The gun row, exactly as the witnessed 0x0D fold leaves it: static spawn
	// pose, occupant back-reference in parent_handle, no compacts ever.
	ns::ClientEntityState &gun = view.state().upsert(kGunHandle);
	gun.type_id = kGunType;
	gun.cls = EntityClass::NoNetworkCallback;
	gun.x = fixed_of(312.6);
	gun.y = fixed_of(384.8);
	gun.z = fixed_of(21.4);
	gun.heading_bam = 454606048; // the live gunner-aim-era heading family
	gun.pitch_bam = 0;
	gun.roll_bam = 0;
	gun.heading_known = true;
	gun.parent_handle = kGunnerHandle; // the occupant back-reference
	gun.parent_pose_valid = false;

	// The gunner row as the infantry-compact fold leaves it: seat-mounted on
	// the gun (carrier + seat-local sample), live aim pitch — the exact live
	// card values from the reproduction session.
	ns::ClientEntityState &gunner = view.state().upsert(kGunnerHandle);
	gunner.type_id = kGunnerType;
	gunner.cls = EntityClass::Infantry;
	gunner.net_has_compact = true;
	gunner.carrier_handle = kGunHandle;
	gunner.mount_bone = 3;
	gunner.net_seat_valid = true;
	gunner.net_seat_local[0] = opennova::network_decompress_fixedpoint(0x59E1);
	gunner.net_seat_local[1] = opennova::network_decompress_fixedpoint(0x0070);
	gunner.net_seat_local[2] = opennova::network_decompress_fixedpoint(0x6E30);
	gunner.heading_bam = 454606048;
	gunner.pitch_bam = 17254540;
	gunner.parent_handle = kGunHandle; // wire-witnessed; compact class => inert

	const int32_t gun_x0 = gun.x, gun_y0 = gun.y, gun_z0 = gun.z;
	const int32_t gun_heading0 = gun.heading_bam;

	// Establish the gunner's composed pose once, then measure drift over five
	// seconds of mover ticks (62.5 Hz). Pre-fix this pair ratcheted ~0.018 u
	// per tick; the tolerance is one centimetre TOTAL.
	view.refresh_carried_entities();
	const int32_t gunner_x0 = gunner.x, gunner_y0 = gunner.y,
			gunner_z0 = gunner.z;
	for (int tick = 0; tick < 312; ++tick)
		view.tick_remote_motion(/*self_handle=*/0xFFFF);

	bool ok = true;
	const double gun_drift = std::sqrt(
			std::pow((gun.x - gun_x0) / 65536.0, 2) +
			std::pow((gun.y - gun_y0) / 65536.0, 2) +
			std::pow((gun.z - gun_z0) / 65536.0, 2));
	const double gunner_drift = std::sqrt(
			std::pow((gunner.x - gunner_x0) / 65536.0, 2) +
			std::pow((gunner.y - gunner_y0) / 65536.0, 2) +
			std::pow((gunner.z - gunner_z0) / 65536.0, 2));
	std::printf("[backref] gun drift %.4f u, heading delta %d BAM; "
	            "gunner drift %.4f u over 312 ticks\n",
	            gun_drift, gun.heading_bam - gun_heading0, gunner_drift);
	ok &= expect(gun.x == gun_x0 && gun.y == gun_y0 && gun.z == gun_z0,
	             "the occupied mount holds its 0x0D spawn position bit-for-bit");
	ok &= expect(gun.heading_bam == gun_heading0,
	             "the occupied mount holds its spawn heading (no occupant glue)");
	ok &= expect(gunner_drift < 0.01,
	             "the seated gunner keeps riding the static mount (no ratchet)");
	ok &= expect(gunner.net_seat_valid,
	             "the seat sample survives the mover loop");
	return ok ? 0 : 1;
}
