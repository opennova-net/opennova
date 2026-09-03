// The impact-scar ring cache and slot writer — see impact_scar.h.
// [orig: Scar_GetEntityCache @0x5CC4C0; Scar_AddEntry @0x5CC830;
//  Scar_AdvanceRingCursor @0x5CC1D0; Scar_ClearEntriesByEntity @0x5ccec0;
//  Scar_TextureForId @0x5CC360; Scar_RadiusForId @0x5CC3B0;
//  Impact_SpawnGlassEffectsOrScar @0x5CF1B0]

#include <runtime/world/impact_scar.h>

#include <cmath>

#include <runtime/world/collision.h>
#include <runtime/world/world.h>

namespace opennova::world {

namespace {

int64_t dot_q16(const int32_t a[3], const int32_t b[3]) {
	return (static_cast<int64_t>(a[0]) * b[0] + static_cast<int64_t>(a[1]) * b[1] +
			static_cast<int64_t>(a[2]) * b[2]) >> 16;
}

void cross_q16(const int32_t a[3], const int32_t b[3], int32_t out[3]) {
	out[0] = static_cast<int32_t>((static_cast<int64_t>(a[1]) * b[2] -
			static_cast<int64_t>(a[2]) * b[1]) >> 16);
	out[1] = static_cast<int32_t>((static_cast<int64_t>(a[2]) * b[0] -
			static_cast<int64_t>(a[0]) * b[2]) >> 16);
	out[2] = static_cast<int32_t>((static_cast<int64_t>(a[0]) * b[1] -
			static_cast<int64_t>(a[1]) * b[0]) >> 16);
}

// The writer's normalise [orig: the fsqrt / 65536 block @0x5CCA74..0x5CCAD7;
// a zero vector stays zero].
void normalize_q16(int32_t v[3]) {
	const double x = static_cast<double>(v[0]);
	const double y = static_cast<double>(v[1]);
	const double z = static_cast<double>(v[2]);
	const double len = std::sqrt(x * x + y * y + z * z);
	if (len <= 0.0) return;
	const double s = 65536.0 / len;
	v[0] = static_cast<int32_t>(x * s);
	v[1] = static_cast<int32_t>(y * s);
	v[2] = static_cast<int32_t>(z * s);
}

} // namespace

void ScarRing::clear() {
	for (ScarSlot &slot : slots) slot = ScarSlot{};
	cursor = 0;
}

void ScarRing::advance_cursor() {
	// [orig: Scar_AdvanceRingCursor @0x5CC1D0 — a compare-and-reset]
	++cursor;
	if (cursor >= static_cast<uint32_t>(kScarsPerEntity)) cursor = 0;
}

ScarCache::ScarCache() : rings_(static_cast<size_t>(kScarCacheEntities)) {}

ScarRing *ScarCache::ring_for(EntityHandle owner, uint64_t lease) {
	if (!owner.valid()) return &world_;
	ScarRing *free_ring = nullptr;
	for (ScarRing &ring : rings_) {
		if (ring.in_use && ring.owner == owner) {
			if (ring.lease != lease) {
				// The handle was reused by a later spawn: the stale ring is
				// re-leased fresh rather than inherited (the generation fold).
				ring.clear();
				ring.lease = lease;
			}
			return &ring;
		}
		if (!ring.in_use && free_ring == nullptr) free_ring = &ring;
	}
	// [orig: the miss with no free entry @0x5CC979..0x5CC983 — no scar]
	if (free_ring == nullptr) return nullptr;
	free_ring->clear();
	free_ring->owner = owner;
	free_ring->lease = lease;
	free_ring->in_use = true;
	return free_ring;
}

const ScarRing *ScarCache::find(EntityHandle owner) const {
	if (!owner.valid()) return &world_;
	for (const ScarRing &ring : rings_)
		if (ring.in_use && ring.owner == owner) return &ring;
	return nullptr;
}

void ScarCache::clear_entity(EntityHandle owner) {
	if (!owner.valid()) return;
	// The shared ring's slots this entity wrote lose their owner (and with it
	// their liveness) [orig: the 256-slot owner-dword sweep at 0x2BDB7FC].
	for (ScarSlot &slot : world_.slots) {
		if (slot.live && slot.owner == owner) slot = ScarSlot{};
	}
	// The entity's own ring is memset and released [orig: the entity-ring
	// memset in Scar_ClearEntriesByEntity @0x5ccec0].
	for (ScarRing &ring : rings_) {
		if (!ring.in_use || ring.owner != owner) continue;
		ring.clear();
		ring.owner = EntityHandle{};
		ring.lease = 0;
		ring.in_use = false;
		return;
	}
}

void ScarCache::reset() {
	// [orig: Scar_ResetAllCaches @0x5CC760 zeroes both ring kinds]
	for (ScarRing &ring : rings_) {
		ring.clear();
		ring.owner = EntityHandle{};
		ring.lease = 0;
		ring.in_use = false;
	}
	world_.clear();
	world_.owner = EntityHandle{};
	world_.lease = 0;
	world_.in_use = false;
}

int ScarCache::leased_count() const {
	int n = 0;
	for (const ScarRing &ring : rings_)
		if (ring.in_use) ++n;
	return n;
}

void scar_basis(const int32_t normal_q16[3], uint16_t spin_word,
		int32_t out_a[3], int32_t out_b[3]) {
	// The ladder, then the normalise [orig: @0x5CC9DA..0x5CCAD7].
	int32_t t[3];
	scar_tangent(normal_q16[0], normal_q16[1], normal_q16[2], t);
	normalize_q16(t);
	// The bitangent is the cross with the normal [orig: @0x5CCADA], and the
	// tangent flips when the triple product `n . (b x t)` is negative
	// [orig: @0x5CCAE6..0x5CCB37].
	int32_t b[3];
	cross_q16(normal_q16, t, b);
	int32_t bxt[3];
	cross_q16(b, t, bxt);
	if (dot_q16(normal_q16, bxt) < 0) {
		t[0] = -t[0];
		t[1] = -t[1];
		t[2] = -t[2];
	}
	// The spin: the PRNG word << 16 read as a SIGNED BAM32, scaled to radians
	// and fed to fsin/fcos; the Q22 rotation of the pair is
	// `T' = (T * c - B * s) >> 22`, `B' = (T * s + B * c) >> 22`
	// [orig: @0x5CCB50..0x5CCC65 — dbl_7C3608 = 2pi / 2^32]. The low word is
	// zero, so the angle is exactly word * 2pi / 65536 modulo 2pi.
	const double angle = static_cast<double>(scar_spin_radians(spin_word));
	const double c = std::cos(angle);
	const double s = std::sin(angle);
	for (int i = 0; i < 3; ++i) {
		const double ti = static_cast<double>(t[i]);
		const double bi = static_cast<double>(b[i]);
		out_a[i] = static_cast<int32_t>(ti * c - bi * s);
		out_b[i] = static_cast<int32_t>(ti * s + bi * c);
	}
}

bool scar_add_entry(World &world, const ProjectileHit &hit, const Entity &target,
		int scar_type) {
	// The ammo kind: 0 leaves nothing, 2 never reaches the ring fallback
	// [orig: Impact_SpawnGlassEffectsOrScar — the `kind != 0` entry gate and
	//  the kind-2 skip @0x5cf289].
	if (!scar_kind_takes_ring_scar(scar_type)) return false;
	// The def gate [orig: @0x5cf1f7].
	if (!scar_entity_allowed(target.engine_flags, target.item_type, target.item_attrib))
		return false;
	// (The GLASS userpoint leg would run here and skip the ring scar on a
	// match — the one open residual, impact_scar.h.)

	// The slot writer's gates in its order: the water plane @0x5CC865, the
	// ring SELECTION @0x5cc873..0x5cc88c, the husk flag @0x5CC894, the
	// section/face indices and the face flag @0x5CC92B — and only then the
	// ring LOOKUP [orig: Scar_GetEntityCache @0x5cc96f..0x5cc983, after the
	//  face gate], so a rejected hit never leases one of the 128 rings.
	if (hit.position_q16.z <= world.env.water_z) return false;
	const bool entity_local = scar_uses_entity_ring(target.handle.pool(), target.item_attrib);
	if ((target.engine_flags & kEntityFlagHusk) != 0u) return false;
	if (hit.section_index < 0 || hit.face_index < 0) return false;
	if ((hit.material_flags & 0x400u) != 0u) return false;
	ScarRing *ring = entity_local
			? world.out.scars.ring_for(target.handle, target.registry_spawn_id)
			: &world.out.scars.world_ring();
	if (ring == nullptr) return false;

	// The id by surface, the radius by id [orig: @0x5CF295; Scar_RadiusForId
	// @0x5CC3B0].
	const int scar_id = scar_id_for_surface(hit.surface_type);
	const int32_t radius = scar_radius_q16(scar_id);

	// The slot's frame. An entity ring stores section-local coordinates: the
	// world hit point through the TRANSPOSED live section matrix and the
	// model-local normal [orig: @0x5ccc99..0x5ccca5; the normal read
	// @0x5cc938..0x5cc960]; the shared ring keeps the world point and the
	// normal rotated by the live matrix [orig: @0x5cc96f]. The trace already
	// rotated the face normal, so the entity-local case un-rotates it. Without
	// a collision instance (headless worlds) the world frame stands in.
	int32_t pos[3] = {hit.position_q16.x, hit.position_q16.y, hit.position_q16.z};
	int32_t normal[3] = {hit.normal_q16.x, hit.normal_q16.y, hit.normal_q16.z};
	if (entity_local && world.collision != nullptr) {
		CollisionMatrix section;
		if (world.collision->entity_section_matrix(world, target.handle,
		                                           hit.section_index, section)) {
			CollisionMatrix inverse;
			section.invert_into(inverse);
			const int32_t world_pos[3] = {pos[0], pos[1], pos[2]};
			const int32_t world_normal[3] = {normal[0], normal[1], normal[2]};
			inverse.transform_point(world_pos, pos);
			inverse.rotate_point(world_normal, normal);
		}
	}

	// The spin word is drawn for every scar [orig: @0x5ccb50], the texture
	// word only for the normal scar at the slot write [orig: the
	// Scar_TextureForId call feeding slot @52 — `first + PRNG_Next16() %
	// (last - first + 1)` when last - first > 0 @0x5CC393; id 18's single
	// strip draws nothing].
	const uint16_t spin = world.next_prng16();
	int32_t axis_a[3];
	int32_t axis_b[3];
	scar_basis(normal, spin, axis_a, axis_b);
	int strip = kScarGlassFallbackTextureStrip;
	if (scar_needs_texture_roll(scar_id)) {
		strip = kScarNormalTextureFirst +
				scar_texture_index(scar_id, world.next_prng16());
	}

	ScarSlot &slot = ring->slots[ring->cursor];
	slot = ScarSlot{};
	for (int i = 0; i < 3; ++i) {
		slot.normal[i] = normal[i];
		slot.axis_a[i] = axis_a[i];
		slot.axis_b[i] = axis_b[i];
		slot.pos[i] = pos[i];
	}
	slot.radius_q16 = radius;
	slot.texture = static_cast<uint8_t>(strip);
	slot.owner = target.handle;
	slot.building = target.item_type == kScarItemTypeBuilding; // [orig: def+92 == 5 @60]
	slot.bone = static_cast<uint8_t>(hit.section_index < 256 ? hit.section_index : 0);
	slot.live = true;
	ring->advance_cursor();
	return true;
}

} // namespace opennova::world
