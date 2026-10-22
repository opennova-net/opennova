// The takeover dialog's feed -- see takeover_status_feed.h.

#include <runtime/inmatch/takeover_status_feed.h>

#include <cmath>
#include <cstdint>

#include <base/io/bam.h>
#include <base/io/crt_ftol.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/replication/client_state.h>
#include <runtime/world/entity.h>
#include <runtime/world/world.h>

namespace opennova::inmatch {

namespace {

// |v| as `cdq; xor; sub` leaves it: INT32_MIN stays itself.
int32_t abs32(int32_t v) {
	return static_cast<int32_t>((static_cast<uint32_t>(v) ^ static_cast<uint32_t>(v >> 31)) -
			static_cast<uint32_t>(v >> 31));
}

} // namespace

bool collect_takeover_status(const RoleView &view, const world::SpawnZoneRegistry &zones,
		hud::HudTakeoverFacts &out) {
	out = hud::HudTakeoverFacts{};
	if (view.kernel == nullptr || view.runtime == nullptr) return false;
	const world::World &w = view.kernel->world;
	const world::Entity *local = w.registry.get(w.cached.local_player);
	if (local == nullptr) return false;
	const ClientRuntime &runtime = *view.runtime;
	const int32_t tx = local->position.x.q16;
	const int32_t ty = local->position.y.q16;
	const int32_t tz = local->position.z.q16;

	int32_t best = 0x40000000; // [orig: @0x537F8F]
	const ClientRuntime::ZoneState::Entry *found = nullptr;
	const world::Entity *found_zone = nullptr;
	for (const auto &pair : runtime.zone_states()) {
		const world::Entity *zone = w.registry.get(world::EntityHandle{ pair.first });
		if (zone == nullptr) continue;
		const ClientRuntime::ZoneState::Entry &e = pair.second.entry;
		// The 2-D length over the wrapped word deltas, the min against
		// flt_7C19E0, ftol; the height's absolute word delta
		// [orig: @0x537FC0..0x538001, @0x538003..0x53800E].
		const double dx = static_cast<double>(io::bam_sub(zone->position.x.q16, tx));
		const double dy = static_cast<double>(io::bam_sub(zone->position.y.q16, ty));
		double length = std::sqrt(dx * dx + dy * dy);
		if (!(length <= 2147418112.0)) length = 2147418112.0;
		const int32_t horizontal = io::retail_ftol_sse2(length);
		const int32_t height = abs32(io::bam_sub(zone->position.z.q16, tz));
		// Bit 0: a running entry [orig: @0x538017..0x53802F].
		if (!(e.window_active || e.value_active)) continue;
		if (e.value_active && e.value_target == e.value_limit) continue;
		// Bit 1: the cylinder, signed [orig: `movzx eax, word [esi+15Eh]; shl
		//  eax, 10h` @0x538038..0x53803F, `jg` @0x538044, the half `cdq; sub;
		//  sar` @0x538046..0x538049, `jg` @0x53804D].
		const int32_t radius = static_cast<int32_t>(static_cast<uint32_t>(zone->zone_radius) << 16);
		if (horizontal > radius || height > radius / 2) continue;
		// Bit 2: the zone's regular slot, and its capture bits for a numbered
		// SpawnPoint [orig: @0x538056..0x53807E].
		const replication::ClientMinimapOverlaySlot *slot =
				replication::find_regular_overlay_slot(runtime.state().minimap, pair.first);
		if (slot == nullptr) continue;
		if ((zone->item_attrib & 0x40000u) != 0 && zone->zone_number != 0 &&
				(slot->source & 0xC0u) == 0)
			continue;
		// The nearest, strictly [orig: `cmp ebp, [esp+var_18]; jge` @0x538080..0x538084].
		if (horizontal >= best) continue;
		best = horizontal;
		found = &e;
		found_zone = zone;
	}
	if (found == nullptr) return false;

	const world::Entity &zone = *found_zone;
	out.present = true;
	out.zone_byte = zone.zone_number;
	out.zone_index = world::spawn_zone_index_of(zones, zone.handle);
	// The 3-D length, the player less the zone, through ftol with no clamp
	// [orig: HUD_DrawTakeoverStatus @0x59B738..0x59B776].
	const double dx = static_cast<double>(io::bam_sub(tx, zone.position.x.q16));
	const double dy = static_cast<double>(io::bam_sub(ty, zone.position.y.q16));
	const double dz = static_cast<double>(io::bam_sub(tz, zone.position.z.q16));
	out.distance_q16 = io::retail_ftol_sse2(std::sqrt(dx * dx + dy * dy + dz * dz));
	out.bound_radius_q16 = zone.bound_radius.q16;
	out.spawn_point_def = (zone.item_attrib & 0x40000u) != 0;
	out.def_type = zone.item_type;
	out.count_owner = found->contest_owner;
	out.count_other = found->contest_other;
	out.team_a = found->mode_a;
	out.team_b = found->mode_b;
	out.window_current = found->window_current;
	out.window_limit = found->window_limit;
	out.window_active = found->window_active;
	out.value_current = found->value_current;
	out.value_target = found->value_target;
	out.value_limit = found->value_limit;
	out.value_rate = found->value_rate;
	out.value_active = found->value_active;
	return true;
}

} // namespace opennova::inmatch
