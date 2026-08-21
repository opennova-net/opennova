// The impact-scar ring cache and slot writer — see impact_scar.h.
// [orig: Scar_GetEntityCache @0x5CC4C0; Scar_AddEntry @0x5CC830;
//  Scar_AdvanceRingCursor @0x5CC1D0; Scar_ClearEntriesByEntity @0x5ccec0;
//  Scar_TextureForId @0x5CC360; Scar_RadiusForId @0x5CC3B0]

#include "world/impact_scar.h"

#include <cmath>

#include "world/collision.h"
#include "world/world.h"

namespace opennova::world {

namespace {

constexpr int32_t kOneQ16 = 0x10000;

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

// The writer's normalise [orig: the fsqrt / 65536 block @0x5CCA74..0x5CCABB;
// a zero vector stays zero @0x5CCAC2..0x5CCACF].
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
	if (!owner.valid()) return &terrain_;
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
	if (!owner.valid()) return &terrain_;
	for (const ScarRing &ring : rings_)
		if (ring.in_use && ring.owner == owner) return &ring;
	return nullptr;
}

void ScarCache::clear_entity(EntityHandle owner) {
	if (!owner.valid()) return;
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
	for (ScarRing &ring : rings_) {
		ring.clear();
		ring.owner = EntityHandle{};
		ring.lease = 0;
		ring.in_use = false;
	}
	terrain_.clear();
	terrain_.owner = EntityHandle{};
	terrain_.lease = 0;
	terrain_.in_use = false;
}

int ScarCache::leased_count() const {
	int n = 0;
	for (const ScarRing &ring : rings_)
		if (ring.in_use) ++n;
	return n;
}

void scar_basis(const int32_t normal_q16[3], uint16_t spin_word,
		int32_t out_a[3], int32_t out_b[3]) {
	// The ladder, then the normalise [orig: @0x5CC9DA..0x5CCABB].
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
	// and fed to fsin/fcos, whose results rotate the tangent pair about the
	// normal [orig: @0x5CCB42..0x5CCB7B — dbl_7C3608 = 2pi / 2^32, the Q22
	//  rotation of the pair]. The low word is zero, so the angle is exactly
	// word * 2pi / 65536 modulo 2pi and the sign fold is a full-turn alias.
	const double angle = static_cast<double>(scar_spin_radians(spin_word));
	const double c = std::cos(angle);
	const double s = std::sin(angle);
	for (int i = 0; i < 3; ++i) {
		const double ti = static_cast<double>(t[i]);
		const double bi = static_cast<double>(b[i]);
		out_a[i] = static_cast<int32_t>(ti * c + bi * s);
		out_b[i] = static_cast<int32_t>(bi * c - ti * s);
	}
}

bool scar_add_entry(World &world, const ProjectileHit &hit, const Entity *target) {
	// The entity-level gates [orig: Scar_AddEntry — the water plane
	// @0x5CC865, the husk flag @0x5CC894] and the face flag [orig: @0x5CC92B].
	const bool husk = target != nullptr &&
			(target->engine_flags & kEntityFlagHusk) != 0u;
	if (!scar_allowed(hit.position_q16.z, world.env.water_z, husk)) return false;
	if ((hit.material_flags & 0x400u) != 0u) return false;

	// The id by surface, the radius by id [orig: @0x5CF295; Scar_RadiusForId
	// @0x5CC3B0].
	const int scar_id = scar_id_for_surface(hit.surface_type);
	const int32_t radius = scar_radius_q16(scar_id);

	// The ring lookup precedes the draws: a cache miss leaves no scar and
	// advances nothing [orig: Scar_GetEntityCache @0x5CC4C0 from @0x5CC979].
	// WITNESS PENDING: the Scar_TextureForId call site relative to the
	// lookup (ported after it).
	const EntityHandle owner = target != nullptr ? target->handle : EntityHandle{};
	const uint64_t lease = target != nullptr ? target->registry_spawn_id : 0u;
	ScarRing *ring = world.scars.ring_for(owner, lease);
	if (ring == nullptr) return false;

	// The texture roll — ONLY the normal scar draws [orig: Scar_TextureForId
	// @0x5CC360 — `first + PRNG_Next16() % (last - first + 1)` when
	// last - first > 0 @0x5CC393; id 18's single strip draws nothing].
	int strip = kScarGlassFallbackTextureStrip;
	if (scar_needs_texture_roll(scar_id)) {
		strip = kScarNormalTextureFirst +
				scar_texture_index(scar_id, world.next_prng16());
	}

	// The spin word, drawn for every scar [orig: @0x5CCB42].
	const uint16_t spin = world.next_prng16();
	const int32_t normal[3] = {hit.normal_q16.x, hit.normal_q16.y, hit.normal_q16.z};

	ScarSlot &slot = ring->slots[ring->cursor];
	slot = ScarSlot{};
	// WITNESS PENDING: the frame of a non-building slot. Retail transforms the
	// face normal for non-building owners @0x5cc938..0x5cc960 and renders
	// those slots through the bone matrix (`bones + bone << 6`); this port
	// stores every slot in WORLD space (the trace's rotated normal + world hit
	// point), which the presenter draws as a world mesh — a moving carrier's
	// scars do not yet follow its pose. Building and terrain slots are exact.
	slot.pos[0] = hit.position_q16.x;
	slot.pos[1] = hit.position_q16.y;
	slot.pos[2] = hit.position_q16.z;
	scar_basis(normal, spin, slot.axis_a, slot.axis_b);
	slot.radius_q16 = radius;
	slot.texture = static_cast<uint8_t>(strip);
	slot.bone = static_cast<uint8_t>(hit.bone_index >= 0 && hit.bone_index < 256
			? hit.bone_index
			: 0);
	slot.building = target != nullptr && target->kind == EntityKind::Building;
	slot.live = true;
	ring->advance_cursor();
	return true;
}

} // namespace opennova::world
