// The impact-scar ring cache and slot writer, and the bullet GLASS leg — see
// impact_scar.h.
// [orig: Scar_GetEntityCache @0x5CC4C0; Scar_AddEntry @0x5CC830;
//  Scar_AdvanceRingCursor @0x5CC1D0; Scar_ClearEntriesByEntity @0x5ccec0;
//  Scar_TextureForId @0x5CC360; Scar_RadiusForId @0x5CC3B0;
//  Impact_SpawnGlassEffectsOrScar @0x5CF1B0;
//  Terrain_SpawnSurfaceEffectsAtUserPoints @0x5CEA90;
//  Scar_ProjectDecalOntoEntity @0x5CE4A0; Scar_HasProjectedDecalAt @0x5CC700;
//  Effect_RollSurfaceEffectProbability @0x5CC1F0]

#include <runtime/world/impact_scar.h>

#include <algorithm>
#include <cmath>

#include <base/crt/crt_rng.h>
#include <base/io/bam.h>
#include <base/io/fixed.h>
#include <base/io/strutil.h>

#include <runtime/world/collision.h>
#include <runtime/world/collision_detail.h>
#include <runtime/world/destruction.h>
#include <runtime/world/world.h>

namespace opennova::world {

namespace {

int64_t dot_q16(const int32_t a[3], const int32_t b[3]) {
	return (static_cast<int64_t>(a[0]) * b[0] + static_cast<int64_t>(a[1]) * b[1] +
			static_cast<int64_t>(a[2]) * b[2]) >> 16;
}

// [orig: Math_FixedPointCrossProduct @0x6134A0 — EACH 64-bit product is
//  rounded (+0x8000, >> 16) BEFORE the subtraction (`imul; add 0x8000; adc;
//  shrd 16` per term @0x6134f7/@0x61353f/@0x613588); the scar basis calls it
//  @0x5CCADA and @0x5CCAE6. Shifting the difference instead is off by one LSB
//  on roughly half the inputs.]
void cross_q16(const int32_t a[3], const int32_t b[3], int32_t out[3]) {
	const auto mul_q16 = [](int32_t x, int32_t y) {
		return static_cast<int32_t>((static_cast<int64_t>(x) * y + 0x8000) >> 16);
	};
	out[0] = mul_q16(a[1], b[2]) - mul_q16(a[2], b[1]);
	out[1] = mul_q16(a[2], b[0]) - mul_q16(a[0], b[2]);
	out[2] = mul_q16(a[0], b[1]) - mul_q16(a[1], b[0]);
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
	// Its projected decals are freed [orig: the Scar_FreeProjectedDecals call
	// @0x5CCF13; the free @0x5CC5FA..0x5CC687].
	decals_.erase(std::remove_if(decals_.begin(), decals_.end(),
			[&](const ProjectedDecalList &list) { return list.owner == owner; }),
			decals_.end());
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

const ProjectedDecalList *ScarCache::find_decals(EntityHandle owner, uint64_t lease) const {
	for (const ProjectedDecalList &list : decals_)
		if (list.owner == owner && list.lease == lease) return &list;
	return nullptr;
}

ProjectedDecalList &ScarCache::decals_for(EntityHandle owner, uint64_t lease) {
	for (ProjectedDecalList &list : decals_) {
		if (list.owner != owner) continue;
		// A handle reused by a later spawn starts a fresh list (the ring fold).
		if (list.lease != lease) {
			list.nodes.clear();
			list.stamp_tick = 0;
			list.lease = lease;
		}
		return list;
	}
	ProjectedDecalList list;
	list.owner = owner;
	list.lease = lease;
	decals_.push_back(std::move(list));
	return decals_.back();
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
	// The projected-decal region is freed with it [orig: Scar_ResetAllCaches
	// @0x5CC760 - j_MemPool_FreeRegion @0x5CC789].
	decals_.clear();
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
	// The spin (scar_spin_radians): its sine and cosine each scaled to Q22 and
	// truncated by `_ftol2_sse`, then every component of the pair rotated with
	// each product shifted on its own, `T' = (c * T >> 22) - (s * B >> 22)`,
	// `B' = (s * T >> 22) + (c * B >> 22)`, each `imul; shrd eax, edx, 16h`.
	// [orig: Scar_AddEntry — the trig @0x5CCB5E..0x5CCB7B (sine @0x5CCB75,
	//  cosine @0x5CCB8E), the tangent @0x5CCBAE..0x5CCC08, the bitangent
	//  @0x5CCC0B..0x5CCC65]
	const double angle = scar_spin_radians(spin_word);
	const int32_t s = static_cast<int32_t>(std::sin(angle) * io::kQ22One);
	const int32_t c = static_cast<int32_t>(std::cos(angle) * io::kQ22One);
	const auto mul22 = [](int32_t a, int32_t b) {
		return static_cast<int32_t>((static_cast<int64_t>(a) * b) >> 22);
	};
	for (int i = 0; i < 3; ++i) {
		out_a[i] = io::bam_sub(mul22(c, t[i]), mul22(s, b[i]));
		out_b[i] = io::bam_add(mul22(s, t[i]), mul22(c, b[i]));
	}
}

// ---------------------------------------------------------------------------
// The bullet GLASS leg
// ---------------------------------------------------------------------------

// [orig: the 192-byte rows @0x841980..0x8433C0, their key @0, userpoint @16,
//  model @32, scar id @176 and radius @180]
const SurfaceMaterialRow kSurfaceMaterialTable[kSurfaceMaterialRowCount] = {
	{"GLASS1", "GLASS", "eurhr2", 3, 0x20000}, {"GLASS1", "GLASS", "eurhr2a", 3, 0x20000},
	{"GLASS1", "GLASS", "eurhr2b", 3, 0x20000}, {"GLASS2", "GLASS02", "eurhr1", 4, 0x20000},
	{"GLASS2", "GLASS02", "eurhr1b", 4, 0x20000}, {"GLASS2", "GLASS02", "eurhr1c", 4, 0x20000},
	{"GLASS2", "GLASS02", "eurhr3", 4, 0x20000}, {"GLASS2", "GLASS02", "eurhr3b", 4, 0x20000},
	{"GLASS2", "GLASS02", "eurhr3c", 4, 0x20000}, {"GLASS1", "GLASS1", "atrm2a", 9, 0x8000},
	{"GLASS2", "GLASS2", "atrm2a", 14, 0x10000}, {"GLASS3", "GLASS3", "atrm2a", 16, 0x40000},
	{"GLASS1", "GLASS1", "atrm2b", 9, 0x8000}, {"GLASS2", "GLASS2", "atrm2b", 14, 0x10000},
	{"GLASS3", "GLASS3", "atrm2b", 15, 0x40000}, {"GLASS1", "GLASS1", "atrm2c", 9, 0x8000},
	{"GLASS2", "GLASS2", "atrm2c", 14, 0x10000}, {"GLASS3", "GLASS3", "atrm2c", 15, 0x40000},
	{"GLASS1", "GLASS1", "ctrblda", 7, 0x20000}, {"GLASS2", "GLASS2", "ctrblda", 8, 0x10000},
	{"GLASS1", "GLASS1", "ctrbldb", 7, 0x20000}, {"GLASS2", "GLASS2", "ctrbldb", 8, 0x10000},
	{"GLASS1", "GLASS01", "airtowr1", 13, 0x20000}, {"GLASS1", "GLASS1", "atrm1a", 9, 0x8000},
	{"GLASS2", "GLASS2", "atrm1a", 10, 0x10000}, {"GLASS3", "GLASS3", "atrm1a", 11, 0x80000},
	{"GLASS4", "GLASS4", "atrm1a", 12, 0x20000}, {"GLASS1", "GLASS1", "atrm1b", 9, 0x8000},
	{"GLASS2", "GLASS2", "atrm1b", 10, 0x10000}, {"GLASS3", "GLASS3", "atrm1b", 11, 0x80000},
	{"GLASS4", "GLASS4", "atrm1b", 12, 0x20000}, {"GLASS1", "GLASS1", "atrm1c", 9, 0x8000},
	{"GLASS2", "GLASS2", "atrm1c", 10, 0x10000}, {"GLASS3", "GLASS3", "atrm1c", 11, 0x80000},
	{"GLASS4", "GLASS4", "atrm1c", 12, 0x20000}};

const SurfaceMaterialRow *find_surface_material_row(const char *key, const std::string &model) {
	for (const SurfaceMaterialRow &row : kSurfaceMaterialTable)
		if (strutil::iequals(row.key, key) && strutil::iequals(row.model, model)) return &row;
	return nullptr;
}

int scar_texture_for_id(World &world, uint8_t id) {
	for (const ScarTableRow &row : kScarTable) {
		if (row.id != id) continue;
		const int32_t range = row.last - row.first;
		if (range > 0) return row.first + static_cast<int32_t>(world.next_prng16()) % (range + 1);
		return row.first;
	}
	return -1;
}

bool effect_roll_surface_probability(World &world, const char *name, bool use_alt) {
	if (name == nullptr || name[0] == '\0') return true;
	const SurfaceEffectProbRow *row = nullptr;
	for (const SurfaceEffectProbRow &r : kSurfaceEffectProbTable) {
		if (strutil::iequals(r.name, name)) {
			row = &r;
			break;
		}
	}
	if (row == nullptr) return true;
	const uint16_t seed = world.next_prng16();
	crt::crt_srand(seed);
	const int32_t roll = static_cast<int32_t>(world.next_prng16()) % 100;
	// The float column times flt_7C4654 (100.0f) at the x87 length, truncated.
	const float p = use_alt ? row->alt : row->main;
	const int32_t limit = static_cast<int32_t>(static_cast<double>(p) * 100.0);
	return roll <= limit;
}

bool scar_has_projected_decal_at(World &world, const Entity &entity, const int32_t origin[3],
		uint8_t scar_id) {
	(void)scar_texture_for_id(world, scar_id); // [orig: @0x5CC705, the result unused]
	const ProjectedDecalList *list =
			world.out.scars.find_decals(entity.handle, entity.registry_spawn_id);
	if (list == nullptr) return false;
	for (const ProjectedDecal &node : list->nodes)
		if (node.origin[0] == origin[0] && node.origin[1] == origin[1] && node.origin[2] == origin[2])
			return true;
	return false;
}

bool scar_project_decal_onto_entity(World &world, const Entity &entity, const ProjectileHit &hit,
		const int32_t origin[3], uint8_t scar_id) {
	// A pool-2 entity, its origin above the water plane [orig: @0x5CE4C9,
	// @0x5CE4E1].
	if (entity.handle.pool() != 2) return false;
	if (origin[2] <= world.env.water_z) return false;
	// The model the leg walks: the husk under Flags & 4 when one is loaded, else
	// the graphic [orig: @0x5CE4EB..0x5CE503].
	const ItemDeathTraits *traits = world.tables.item_death_traits.get(entity.item_id);
	// A promoted husk is the final husk's model (Entity::husk_promoted).
	const bool husk = (entity.engine_flags & kEntityFlagHusk) != 0u && traits != nullptr &&
			husk_model_held(*traits, entity);
	const bool final_husk = husk && entity.husk_promoted;
	if (!husk && !entity.has_graphic_model) return false;
	// The model's collision block (+0xB0) [orig: @0x5CE509..0x5CE511].
	if (traits != nullptr &&
			(final_husk ? traits->final_husk_no_collision_block
			 : husk     ? traits->husk_no_collision_block
			            : traits->graphic_no_collision_block))
		return false;
	// The model's LOD-0 render model, present and not skinned ((+0x20) non-null, its +0x10
	// bit 0 clear) [orig: @0x5CE517..0x5CE526].
	if (traits != nullptr &&
			(final_husk ? traits->final_husk_lod0_refuses_decal
			 : husk     ? traits->husk_lod0_refuses_decal
			            : traits->graphic_lod0_refuses_decal))
		return false;
	// The scar-cache bone callback (+0xAC), which the item load that creates the model binds
	// (never null there),
	// called for the section matrices [orig: @0x5CE52C..0x5CE53E]; the struck section, then
	// its matrix, the array plus 64 bytes a section, must be non-null, so a callback that
	// returns no array refuses section 0 alone [orig: @0x5CE540..0x5CE564]; then the face and
	// its no-scar flag [orig: @0x5CE578..0x5CE5A2, the gates Scar_AddEntry shares].
	if (hit.section_index < 0) return false;
	if (traits != nullptr &&
			(final_husk ? traits->final_husk_scar_bones_null
			 : husk     ? traits->husk_scar_bones_null
			            : traits->graphic_scar_bones_null) &&
			hit.section_index == 0)
		return false;
	if (hit.face_index < 0) return false;
	if ((hit.material_flags & 0x400u) != 0u) return false;
	// The first-hit rule [orig: @0x5CE624].
	if (scar_has_projected_decal_at(world, entity, origin, scar_id)) return false;
	ProjectedDecalList &list = world.out.scars.decals_for(entity.handle, entity.registry_spawn_id);
	list.stamp_tick = world.logic_tick; // [orig: @0x5CE646]
	// The node at the head, then its texture draw [orig: @0x5CE690..0x5CE6A0].
	ProjectedDecal node;
	node.texture = scar_texture_for_id(world, scar_id);
	if (node.texture < 0) return false;
	for (int i = 0; i < 3; ++i) node.origin[i] = origin[i];
	for (const ScarTableRow &row : kScarTable) {
		if (row.id == scar_id) {
			node.radius_q16 = row.radius_q16; // [orig: Scar_RadiusForId @0x5CE6F4]
			break;
		}
	}
	list.nodes.insert(list.nodes.begin(), node);
	return true;
}

int surface_effects_at_userpoints(World &world, const char *key, const Entity &entity,
		const ProjectileHit &hit, int scar_type) {
	// The def's NoScar attribute ends the leg for any def type [orig:
	// @0x5CEAB3..0x5CEABA].
	if ((entity.item_attrib & kItemAttribNoScar) != 0u) return 0;
	const ItemDeathTraits *traits = world.tables.item_death_traits.get(entity.item_id);
	if (traits == nullptr) return 0;
	// The husk under Flags & 4 when one is loaded, else the graphic
	// [orig: @0x5CEABC..0x5CEADB].
	// A promoted husk is the final husk's model (Entity::husk_promoted).
	const bool husk = (entity.engine_flags & kEntityFlagHusk) != 0u && husk_model_held(*traits, entity);
	if (!husk && !entity.has_graphic_model) return 0;
	const bool final_husk = husk && entity.husk_promoted;
	const std::string &model_name = final_husk ? traits->final_husk_name
			: husk ? traits->husk_name : traits->graphic_name;
	const std::vector<ModelUserPoint> &points = final_husk ? traits->final_husk_userpoints
			: husk ? traits->husk_userpoints : traits->graphic_userpoints;
	// The row, then the two texture gates, each a draw when its id spans more
	// than one texture [orig: @0x5CEAF7, @0x5CEB08, @0x5CEB1C].
	const SurfaceMaterialRow *row = find_surface_material_row(key, model_name);
	if (row == nullptr) return 0;
	if (scar_texture_for_id(world, static_cast<uint8_t>(scar_type)) < 0) return 0;
	if (scar_texture_for_id(world, row->scar_id) < 0) return 0;
	// The entity's matrix: the euler rotation with the entity's (else the def's)
	// uniform scale [orig: @0x5CEB5C..0x5CEB99].
	const CollisionMatrix matrix = entity_placement_matrix(entity);
	const int32_t hx = hit.position_q16.x, hy = hit.position_q16.y, hz = hit.position_q16.z;
	const int32_t r = row->radius_q16;
	int matches = 0;
	for (const ModelUserPoint &point : points) {
		if (!strutil::iequals(point.name, row->userpoint)) continue; // [orig: @0x5CEBBE]
		int32_t p[3];
		matrix.transform_point(point.pos_q16, p); // [orig: @0x5CEBF2]
		// The inclusive box on each axis [orig: @0x5CEC0F..0x5CEC65].
		if (hx < io::bam_sub(p[0], r) || hx > io::bam_add(p[0], r)) continue;
		if (hy < io::bam_sub(p[1], r) || hy > io::bam_add(p[1], r)) continue;
		if (hz < io::bam_sub(p[2], r) || hz > io::bam_add(p[2], r)) continue;
		++matches; // [orig: @0x5CEC6B]
		// The origin is the userpoint (+184 set on every row), the decal leg
		// taken (+188 set on every row); no decal added: the next point, no
		// rolls [orig: @0x5CEC72, @0x5CECA2, @0x5CECBD..0x5CECC7].
		if (!scar_project_decal_onto_entity(world, entity, hit, p, row->scar_id)) continue;
		// The point's direction through the matrix, then the four rolls, each
		// ahead of its name test, and a spawn per pass [orig: @0x5CECCD..0x5CECEF;
		// @0x5CED19 / @0x5CED44 / @0x5CED6F / @0x5CED9D; CEffectWorld_SpawnEmitterAtPosition
		// @0x5CED37 / @0x5CED62 / @0x5CED8D / @0x5CEDBB].
		int32_t d[3];
		matrix.rotate_point(point.dir_q16, d);
		for (const char *effect : kSurfaceMaterialEffects) {
			if (!effect_roll_surface_probability(world, effect, false) || effect[0] == '\0') continue;
			VehicleEffectEvent spawn;
			spawn.effect = effect;
			spawn.position = Vec3{float(from_fixed(p[0])), float(from_fixed(p[1])), float(from_fixed(p[2]))};
			spawn.direction = Vec3{float(from_fixed(d[0])), float(from_fixed(d[1])), float(from_fixed(d[2]))};
			spawn.source_tick = world.logic_tick;
			world.out.vehicle_effects.push_back(std::move(spawn));
		}
	}
	return matches;
}

bool scar_add_entry(World &world, const ProjectileHit &hit, const Entity &target,
		int scar_type) {
	// The ammo kind: 0 leaves nothing [orig: Impact_SpawnGlassEffectsOrScar —
	// the `kind != 0` entry gate].
	if (scar_type == kScarKindNone) return false;
	// The def gate [orig: @0x5cf1f7].
	if (!scar_entity_allowed(target.engine_flags, target.item_type, target.item_attrib))
		return false;
	// The GLASS chain: the first key that matched ends the impact [orig:
	// @0x5CF217 / @0x5CF23A / @0x5CF259 / @0x5CF278].
	static constexpr const char *kGlassKeys[4] = {"GLASS1", "GLASS2", "GLASS3", "GLASS4"};
	for (const char *key : kGlassKeys)
		if (surface_effects_at_userpoints(world, key, target, hit, scar_type) != 0) return true;
	// Kind 2 never reaches the ring [orig: @0x5cf289].
	if (!scar_kind_takes_ring_scar(scar_type)) return false;

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
	// normal rotated by the live matrix [orig: @0x5cc96f]. The transposed
	// matrix only NEGATES the translation, so the point takes the
	// translate-then-rotate transform, local = R^T (p - t); rotating first
	// would land every entity-ring scar hundreds of units off its owner
	// [orig: Matrix_Transpose3x3WithNegateCol3 @0x6136d0 ->
	// Math_TransformPointWithTranslation22 @0x412f60]. The trace already
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
			detail::transform_translate_then_rotate(inverse.m, world_pos, pos);
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
