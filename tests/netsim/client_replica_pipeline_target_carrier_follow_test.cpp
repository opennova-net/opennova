// A compact-less TARGET-carried emplacement must ride its DRIVING structural
// carrier (the 2026-08-05 live repro, retail 01TR host -> OpenNova joiner: a
// Dune Buggy's "50 cal. (non-armored)" 0x0D row — target=<buggy>, bone 0,
// compact_revision forever 0 — stayed floating at spawn while the hull drove
// away; parked alignment was already correct).
//
// Retail keeps such a child glued CLIENT-SIDE with no wire records of its own:
// the 'ewep' class MOVE function recomposes the child from groundEntity
// (entity+40) every tick — the slot the 0x0D TARGET seeds [orig: move-fn table
// 'ewep' row @0x82abe0 -> Entity_UpdateTransformAndTurret @0x440ca0 —
// groundEntity read @0x440cbf, carrier-matrix bone compose @0x44109d with
// position+Euler adoption @0x4410dd, no-bone verbatim carrier-pose adoption
// @0x4410ea..0x4411bc; target -> groundEntity resolve @0x4332bc, store @0x4332d7].
// The spawn parentHandle is the occupant back-ref (+368) and must stay inert
// (D-NET-195).
//
// This test drives the REAL wire path: a 0x0D pool-spawn batch (encoded then
// decoded) spawns hull + target-carried gun, then §5.13 vehicle compacts drive
// the hull away and through a heading change. The gun must recompose onto the
// live hull every tick — rigid carrier-local offset, rotating with the hull.

#include <net/netsim/client_replica_pipeline.h>

#include <net/npwire/entity_class.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace {

namespace nw = opennova;
namespace ns = opennova::netsim;

constexpr uint16_t kHullHandle = 0x1006; // pool 1 slot 6 (the buggy)
constexpr uint16_t kGunHandle = 0x1017;  // pool 1 slot 23 (its mounted 50cal)
constexpr uint16_t kHullType = 0x0248;   // vehicle-class carrier
constexpr uint16_t kGunType = 0x058B;    // "50 cal. (non-armored)" (ewep)

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

int32_t fixed_of(double v) { return static_cast<int32_t>(v * 65536.0); }

nw::FrameUpdate hull_frame(int32_t ax, int32_t ay, int32_t az, int32_t x,
                           int32_t y, int16_t euler_z_high) {
	nw::FrameUpdate fu;
	fu.flags2 = 0;
	fu.mount_handle = 0xFFFF;
	fu.health = 100;
	fu.anchor_x = ax;
	fu.anchor_y = ay;
	fu.anchor_z = az;
	nw::FrameUpdateRecord r;
	r.handle = kHullHandle;
	r.type_id = kHullType;
	r.cls = nw::EntityClass::Vehicle;
	r.vehicle.parent_slot_handle = 0xFFFF;
	r.vehicle.pos_x_compressed = nw::network_compress_fixedpoint(x - ax);
	r.vehicle.pos_y_compressed = nw::network_compress_fixedpoint(y - ay);
	r.vehicle.pos_z_compressed = nw::network_compress_fixedpoint(0);
	r.vehicle.euler_z = euler_z_high;
	r.vehicle.flags_byte = 0; // live form
	r.vehicle.is_dead_pose = false;
	r.vehicle.health_word = 100;
	r.vehicle.weapon_aim_y = nw::network_compress_fixedpoint(8192);
	fu.records.push_back(r);
	return fu;
}

// World-space expected gun position for the captured carrier-local offset,
// composed against the hull's LIVE row pose the way the pipeline does.
double offset_err_m(const ns::ClientEntityState &gun,
                    const ns::ClientEntityState &hull, int32_t lx, int32_t ly,
                    int32_t lz) {
	const nw::WorldPose posed = nw::network_transform_local_to_world(
			lx, ly, lz, hull.x, hull.y, hull.z,
			static_cast<uint32_t>(hull.heading_bam),
			static_cast<uint32_t>(hull.pitch_bam),
			static_cast<uint32_t>(hull.roll_bam));
	return std::sqrt(std::pow((gun.x - posed.x) / 65536.0, 2) +
	                 std::pow((gun.y - posed.y) / 65536.0, 2) +
	                 std::pow((gun.z - posed.z) / 65536.0, 2));
}

} // namespace

int main() {
	ns::ClientReplicaPipeline view;
	view.set_remote_motion_mode(true);
	view.set_item_class_resolver([](uint16_t type)
			-> ns::ClientReplicaPipeline::ItemClassResolution {
		if (type == kGunType) return nw::EntityClass::NoNetworkCallback;
		if (type == kHullType) return nw::EntityClass::Vehicle;
		return std::nullopt;
	});

	// --- The 0x0D world page, through the real encode/decode path: the hull,
	// then the gun with target=<hull> (parent stays 0xFFFF: unoccupied mount)
	// and the live-witnessed bone byte 0.
	const int32_t hull_x0 = fixed_of(100.0), hull_y0 = fixed_of(200.0),
			hull_z0 = fixed_of(10.0);
	const int32_t gun_dx = fixed_of(-1.20), gun_dy = fixed_of(0.35),
			gun_dz = fixed_of(0.90);
	nw::PoolSpawnBatch batch;
	{
		nw::PoolSpawnRecord hull;
		hull.slot_id = kHullHandle;
		hull.item_type_id = kHullType;
		hull.entity_name = "Drivable Dune Buggy";
		hull.pos_x = hull_x0;
		hull.pos_y = hull_y0;
		hull.pos_z = hull_z0;
		hull.euler_z = 0x20000000; // spawn heading
		batch.records.push_back(hull);
		nw::PoolSpawnRecord gun;
		gun.slot_id = kGunHandle;
		gun.item_type_id = kGunType;
		gun.entity_name = "50 cal. (non-armored)";
		gun.pos_x = hull_x0 + gun_dx; // server-computed absolute mount pose
		gun.pos_y = hull_y0 + gun_dy;
		gun.pos_z = hull_z0 + gun_dz;
		gun.euler_z = 0x20000000;
		gun.bone_byte = 0;             // live-witnessed record
		gun.target_handle = kHullHandle; // -> groundEntity (+40)
		gun.parent_handle = 0xFFFF;    // no occupant
		batch.records.push_back(gun);
	}
	view.apply(nw::s2c::POOL_SPAWN, nw::encode_pool_spawn_batch(batch));

	const ns::ClientEntityState *gun = view.state().find(kGunHandle);
	const ns::ClientEntityState *hull = view.state().find(kHullHandle);
	bool ok = true;
	ok &= expect(gun != nullptr && hull != nullptr,
	             "both 0x0D rows decode into the view");
	if (!ok) return 1;
	ok &= expect(gun->target_handle == kHullHandle,
	             "the decoded 0x0D target stages the structural carrier");

	// Captured carrier-local offset from the spawn absolutes (heading equal at
	// spawn, so world delta == carrier-local delta up to trig quantization).
	const nw::WorldPose local0 = nw::network_transform_world_to_local(
			gun->x, gun->y, gun->z, hull->x, hull->y, hull->z,
			static_cast<uint32_t>(hull->heading_bam),
			static_cast<uint32_t>(hull->pitch_bam),
			static_cast<uint32_t>(hull->roll_bam));

	// --- Drive the hull away: 8-tick compact cadence, +0.20 m/tick east for
	// 400 ticks (~80 m), then a 90-degree heading change and 200 more ticks
	// north. The gun must stay glued through both legs.
	const int32_t ax = hull_x0, ay = hull_y0, az = hull_z0;
	int32_t true_x = hull_x0, true_y = hull_y0;
	double max_err = 0.0;
	for (int t = 0; t < 400; ++t) {
		if (t % 8 == 0) {
			view.apply(nw::s2c::PER_FRAME_UPDATE,
			           nw::encode_frame_update(
			               hull_frame(ax, ay, az, true_x, true_y, 0x2000)));
		}
		true_x += fixed_of(0.20);
		view.tick_remote_motion(/*self_handle=*/0xFFFF);
		max_err = std::max(max_err, offset_err_m(*gun, *hull, local0.x,
		                                         local0.y, local0.z));
	}
	const double drive_err = offset_err_m(*gun, *hull, local0.x, local0.y,
	                                      local0.z);
	for (int t = 0; t < 200; ++t) {
		if (t % 8 == 0) {
			view.apply(nw::s2c::PER_FRAME_UPDATE,
			           nw::encode_frame_update(
			               hull_frame(ax, ay, az, true_x, true_y, 0x6000)));
		}
		true_y += fixed_of(0.20);
		view.tick_remote_motion(0xFFFF);
	}
	const double turn_err = offset_err_m(*gun, *hull, local0.x, local0.y,
	                                     local0.z);
	const double hull_moved = std::sqrt(
			std::pow((hull->x - hull_x0) / 65536.0, 2) +
			std::pow((hull->y - hull_y0) / 65536.0, 2));
	std::printf("[target-follow] hull moved %.2f m; gun offset error: "
	            "drive %.4f m (max %.4f), post-turn %.4f m\n",
	            hull_moved, drive_err, max_err, turn_err);

	ok &= expect(hull_moved > 50.0, "the hull actually drove away");
	// Pre-fix the gun held its spawn position: offset error == distance driven
	// (~80 m). Post-fix it recomposes from the live hull every tick; the only
	// slack is compression quantization + the local/world trig round trip.
	ok &= expect(drive_err < 0.05,
	             "the target-carried gun rides the driving hull (drive leg)");
	ok &= expect(max_err < 0.05,
	             "the gun never detaches mid-drive (per-tick recompose)");
	ok &= expect(turn_err < 0.05,
	             "the captured offset rotates with the hull heading (turn leg)");
	// The follow adopts the carrier attitude composition: with a zero captured
	// heading delta the gun's heading must track the hull's live heading.
	ok &= expect(gun->heading_bam == hull->heading_bam,
	             "the gun heading tracks the hull heading (verbatim adoption)");

	// --- The pure-client 128-tick stale-carrier sweep [orig:
	// Entity_UpdateTransformAndTurret @0x440ca0, the sweep @0x440d41..
	// 0x440e2f]: a child whose structural carrier never resolves requests
	// BOTH rows over C2S 0x0F after a 128-tick run, then locally destroys
	// itself pending the authority re-spawn.
	{
		constexpr uint16_t kOrphanGun = 0x1018;
		constexpr uint16_t kMissingHull = 0x1030; // never spawned
		nw::PoolSpawnBatch orphan_batch;
		nw::PoolSpawnRecord orphan;
		orphan.slot_id = kOrphanGun;
		orphan.item_type_id = kGunType;
		orphan.entity_name = "50 cal. (orphaned)";
		orphan.pos_x = hull_x0;
		orphan.pos_y = hull_y0;
		orphan.pos_z = hull_z0;
		orphan.euler_z = 0;
		orphan.bone_byte = 0;
		orphan.target_handle = kMissingHull;
		orphan.parent_handle = 0xFFFF;
		orphan_batch.records.push_back(orphan);
		view.apply(nw::s2c::POOL_SPAWN,
		           nw::encode_pool_spawn_batch(orphan_batch));
		ok &= expect(view.state().find(kOrphanGun) != nullptr,
		             "the orphaned gun materializes");
		(void)view.drain_carrier_repair_requests(); // clear any prior state
		for (int t = 0; t < 127; ++t) view.tick_remote_motion(0xFFFF);
		ok &= expect(view.state().find(kOrphanGun) != nullptr,
		             "127 unresolved ticks keep the child (inside the window)");
		ok &= expect(view.drain_carrier_repair_requests().empty(),
		             "no repair request before the 128-tick boundary");
		view.tick_remote_motion(0xFFFF);
		const std::vector<uint16_t> repairs =
				view.drain_carrier_repair_requests();
		bool asked_carrier = false, asked_child = false;
		for (uint16_t h : repairs) {
			if (h == kMissingHull) asked_carrier = true;
			if (h == kOrphanGun) asked_child = true;
		}
		ok &= expect(asked_carrier && asked_child,
		             "the sweep queues C2S 0x0F for carrier AND child");
		ok &= expect(view.state().find(kOrphanGun) == nullptr,
		             "the sweep locally destroys the child pending re-spawn");
	}

	// --- The dead-carrier leg. Retail's client HIDES the child in place
	// (carrier Flags & 2 -> child Flags |= 1 @0x440cdb..0x440cdd) pending the
	// authority's destroy transaction; our decoded view RETIRES the subtree
	// instead — presentation-equivalent, and the authority truth on this seam
	// (the loopback-identity test pins the same retire through the parent
	// relation). A zero health word is the death signal; the wire flags bit 1
	// alone must NOT retire (an overloaded spawn/movement gate).
	{
		nw::FrameUpdate flagged = hull_frame(ax, ay, az, true_x, true_y, 0x6000);
		flagged.records[0].vehicle.flags_byte = 0x02; // gate bit, health alive
		view.apply(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(flagged));
		view.tick_remote_motion(0xFFFF);
		ok &= expect(view.state().find(kGunHandle) != nullptr,
		             "the overloaded flags bit alone never retires the child");
		nw::FrameUpdate dead = hull_frame(ax, ay, az, true_x, true_y, 0x6000);
		dead.records[0].vehicle.health_word = 0;
		dead.records[0].vehicle.is_dead_pose = true;
		view.apply(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(dead));
		view.tick_remote_motion(0xFFFF);
		ok &= expect(view.state().find(kGunHandle) == nullptr,
		             "a zero-health carrier retires the target-carried child");
	}
	return ok ? 0 : 1;
}
