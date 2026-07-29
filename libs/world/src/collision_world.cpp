#include "world/collision.h"

// Split out of collision.cpp (quality campaign W3-2). Motion only — every body is
// unchanged, and each original-code citation moved with the code it annotates.
//
// CollisionWorld's model/instance registry and the per-tick proximity table build
// — what the queries above are pointed at.

#include <algorithm>
#include <cmath>

#include "collision_detail.h"

#include "world/world.h"

namespace opennova::world {

using namespace detail; // the shared fixed-point helpers, unqualified as before

// ----------------------------------------------------------------------------
// CollisionWorld
// ----------------------------------------------------------------------------

int32_t CollisionWorld::add_model(CollisionModel model) {
    model.finalize_sections();
    models_.push_back(std::move(model));
    // CollisionTargetView retains a pointer into models_. A push can reallocate
    // that vector even though every existing model id remains stable.
    invalidate_trace_views();
    return static_cast<int32_t>(models_.size()) - 1;
}

const CollisionModel *CollisionWorld::model(int32_t id) const {
    if (id < 0 || id >= static_cast<int32_t>(models_.size())) return nullptr;
    return &models_[id];
}

void CollisionWorld::assign_entity(EntityHandle h, int32_t model_id,
                                   uint64_t registry_spawn_id) {
    if (!h.valid() || model_id < 0 || model_id >= static_cast<int32_t>(models_.size())) return;
    int32_t husk = -1;
    const auto existing = instances_.find(h.packed);
    if (existing != instances_.end() &&
        (registry_spawn_id == 0 ||
         existing->second.registry_spawn_id == registry_spawn_id)) {
        husk = existing->second.husk_model_id;
    }
    instances_[h.packed] = Instance{model_id, husk, registry_spawn_id};
    invalidate_trace_view(h);
}

void CollisionWorld::remove_entity_instance(EntityHandle h) {
    if (!h.valid()) return;
    instances_.erase(h.packed);
    invalidate_trace_view(h);
}

void CollisionWorld::assign_entity_husk(EntityHandle h, int32_t husk_model_id) {
    if (!h.valid() || husk_model_id < 0 ||
        husk_model_id >= static_cast<int32_t>(models_.size()))
        return;
    auto it = instances_.find(h.packed);
    if (it == instances_.end()) return; // husk stages ride an existing instance
    it->second.husk_model_id = husk_model_id;
    invalidate_trace_view(h);
}

void CollisionWorld::set_section_matrix_provider(
        ICollisionSectionMatrixProvider *provider) {
    if (section_matrix_provider_ == provider) return;
    section_matrix_provider_ = provider;
    invalidate_trace_views();
}

const CollisionWorld::Instance *CollisionWorld::live_instance(
        const World &world, EntityHandle h) const {
    const auto it = instances_.find(h.packed);
    if (it == instances_.end()) return nullptr;
    const Entity *entity = world.registry.get(h);
    if (entity == nullptr) return nullptr;
    if (it->second.registry_spawn_id != 0 &&
        it->second.registry_spawn_id != entity->registry_spawn_id)
        return nullptr;
    return &it->second;
}

bool CollisionWorld::has_instance(const World &world, EntityHandle h) const {
    return live_instance(world, h) != nullptr;
}

bool CollisionWorld::publish_entity_section_matrices(
    EntityHandle h, std::vector<CollisionMatrix> matrices) {
    auto it = instances_.find(h.packed);
    if (it == instances_.end()) return false;
    const CollisionModel *m = model(it->second.model_id);
    if (m == nullptr || matrices.size() != m->sections.size()) return false;
    it->second.section_matrices = std::move(matrices);
    invalidate_trace_view(h);
    return true;
}

void CollisionWorld::clear_entity_section_matrices(EntityHandle h) {
    auto it = instances_.find(h.packed);
    if (it != instances_.end()) it->second.section_matrices.clear();
    invalidate_trace_view(h);
}

bool CollisionWorld::has_instance(EntityHandle h) const {
    return instances_.count(h.packed) != 0;
}

int32_t CollisionWorld::candidate_count(EntityHandle h) const {
    auto it = candidates_.find(h.packed);
    return it == candidates_.end() ? 0 : it->second.count;
}

const EntityHandle *CollisionWorld::candidate_slice(EntityHandle h, int32_t &count_out) const {
    auto it = candidates_.find(h.packed);
    if (it == candidates_.end() || it->second.count <= 0) {
        count_out = 0;
        return nullptr;
    }
    count_out = it->second.count;
    return arena_.data() + it->second.start;
}

CollisionWorld::StaticSlotView CollisionWorld::static_slot(int32_t i) const {
    StaticSlotView v;
    if (i < 0 || i >= static_count_) return v;
    const StaticSlot &s = statics_[i];
    v.x = s.x;
    v.y = s.y;
    v.z = s.z;
    v.radius = s.radius;
    v.h = s.h;
    return v;
}

const CollisionModel *CollisionWorld::model_for(
        const World &world, EntityHandle h) const {
    const Instance *instance = live_instance(world, h);
    return instance != nullptr ? model(instance->model_id) : nullptr;
}

const CollisionModel *CollisionWorld::model_for(EntityHandle h) const {
    const auto it = instances_.find(h.packed);
    return it != instances_.end() ? model(it->second.model_id) : nullptr;
}

bool CollisionWorld::ensure_entity_instance(World &world, EntityHandle h) {
    auto existing = instances_.find(h.packed);
    if (existing != instances_.end()) {
        const Entity *entity = world.registry.get(h);
        if (entity != nullptr &&
            (existing->second.registry_spawn_id == 0 ||
             entity->registry_spawn_id ==
                     existing->second.registry_spawn_id)) {
            return true;
        }
        // The packed slot was despawned/reused. Never let its old intact or
        // husk model satisfy a query for the new registry lifetime.
        instances_.erase(existing);
        invalidate_trace_view(h);
    }
    if (section_matrix_provider_ == nullptr) return false;
    if (!section_matrix_provider_->ensure_collision_instance(world, h)) return false;
    return has_instance(world, h);
}

namespace detail { // was anonymous; the entity position/radius set is declared in collision_detail.h

// Entity position in 16.16 engine units (registry entities store float mission units).
void entity_pos_fixed(const Entity &e, int32_t out[3]) {
    out[0] = to_fixed(e.position.x);
    out[1] = to_fixed(e.position.y);
    out[2] = to_fixed(e.position.z);
}

// Collision-model fallback for hosts/tests that have not stamped entity+0.
// The real proximity-table radius is the model-header bound on Entity; deriving
// max |collision AABB corner| is only the best available fallback.
int32_t entity_bound_radius(const CollisionWorld &cw, const CollisionModel *model,
                            int32_t uniform_scale_q16) {
    (void)cw;
    if (model == nullptr) return 0x10000;
    uint64_t radius = model->fallback_bound_radius_q16;

    // Zero is the ordinary unscaled sentinel. A host-stamped Entity bound is
    // already effective; only this model-derived fallback needs matrix scale.
    if (uniform_scale_q16 != 0) {
        const uint64_t scale = int32_magnitude(uniform_scale_q16);
        radius = (radius * scale + 0xFFFFu) >> 16; // ceil Q16 multiplication
    }
    constexpr uint64_t kMaxSafeRadius = 2147352576u; // leaves static-table pad headroom
    if (radius > kMaxSafeRadius) radius = kMaxSafeRadius;
    return static_cast<int32_t>(radius);
}

int32_t entity_proximity_radius(const CollisionWorld &cw, const Entity &e,
                                const CollisionModel *fallback_model) {
    if (e.bound_radius > 0.0f) return to_fixed(e.bound_radius);
    return entity_bound_radius(cw, fallback_model, e.uniform_scale_q16);
}

} // namespace detail

void CollisionWorld::replace_projectile_person_proxies(
        std::vector<ProjectilePersonProxy> proxies,
        uint16_t local_player_wire_handle) {
    proxies.erase(
        std::remove_if(proxies.begin(), proxies.end(),
                       [](const ProjectilePersonProxy &proxy) {
                           return proxy.wire_handle == EntityHandle::kInvalid;
                       }),
        proxies.end());
    std::stable_sort(
        proxies.begin(), proxies.end(),
        [](const ProjectilePersonProxy &a, const ProjectilePersonProxy &b) {
            return a.wire_handle < b.wire_handle;
        });
    projectile_person_proxies_ = std::move(proxies);
    projectile_local_player_wire_handle_ = local_player_wire_handle;
}

void CollisionWorld::replace_projectile_dynamic_proxies(
        std::vector<ProjectileDynamicProxy> proxies) {
    proxies.erase(
        std::remove_if(proxies.begin(), proxies.end(),
                       [](const ProjectileDynamicProxy &proxy) {
                           return proxy.wire_handle == EntityHandle::kInvalid;
                       }),
        proxies.end());
    std::stable_sort(
        proxies.begin(), proxies.end(),
        [](const ProjectileDynamicProxy &a, const ProjectileDynamicProxy &b) {
            return a.wire_handle < b.wire_handle;
        });
    projectile_dynamic_proxies_ = std::move(proxies);
}

void CollisionWorld::set_trace_profile_enabled(bool enabled) {
    if (trace_profile_enabled_ == enabled) return;
    trace_profile_enabled_ = enabled;
    trace_profile_ = TraceProfile{};
}

void CollisionWorld::invalidate_trace_view(EntityHandle h) {
    if (h.valid()) trace_view_cache_.erase(h.packed);
}

void CollisionWorld::invalidate_trace_views() {
    trace_view_cache_.clear();
}

void CollisionWorld::build_tick_tables(World &world) {
    if (trace_profile_enabled_) trace_profile_ = TraceProfile{};
    build_tables(world, true);
}

void CollisionWorld::build_initial_tables(World &world) {
    build_tables(world, false);
}

void CollisionWorld::build_tables(World &world, bool advance_candidate_slices) {
    // Every pool-table publication is a new trace-view epoch, including the
    // mission-initial and registry-refresh paths that do not advance retail's
    // 17-tick candidate-slice cadence.
    invalidate_trace_views();
    tick_tables_built_ = true;
    // Packed pool/slot handles are reused. Remove every binding whose recorded
    // lifetime no longer names the registry occupant before any proximity,
    // contact, or occlusion consumer can observe its old model or husk.
    for (auto it = instances_.begin(); it != instances_.end();) {
        const EntityHandle h{it->first};
        const Entity *entity = world.registry.get(h);
        if (entity == nullptr ||
            (it->second.registry_spawn_id != 0 &&
             it->second.registry_spawn_id != entity->registry_spawn_id)) {
            it = instances_.erase(it);
        } else {
            ++it;
        }
    }

    // --- pool-2 statics: buildings first, then the rest. [orig: 0x4b9430] ---
    statics_.clear();
    static_building_count_ = 0;
    auto push_static = [&](const Entity &e) {
        // The original's count saturates at 1199 — the 1200th slot is written
        // but never counted, so 1199 is the effective cap. [orig: the
        // `count < 1199` post-increment gate @ 0x4b94cb / 0x4b955f]
        if (statics_.size() >= 1199) return;
        auto it = instances_.find(e.handle.packed);
        const CollisionModel *attached =
            it != instances_.end() ? model(it->second.model_id) : nullptr;
        // Projectile proximity also retains the bounded compatibility entry for
        // an item whose graphic could not resolve a collision instance. Other
        // collision consumers still reject it later at target_view().
        if (attached == nullptr && e.bound_radius <= 0.0f) return;
        StaticSlot s;
        int32_t p[3];
        entity_pos_fixed(e, p);
        const int32_t radius =
            entity_proximity_radius(*this, e, attached) + 111876;
        s.x = static_cast<uint16_t>((static_cast<uint32_t>(p[0]) + 0x8000u) >> 16);
        s.y = static_cast<uint16_t>((static_cast<uint32_t>(p[1]) + 0x8000u) >> 16);
        s.z = static_cast<uint16_t>((static_cast<uint32_t>(p[2]) + 0x8000u) >> 16);
        s.radius = static_cast<uint16_t>(static_cast<uint32_t>(radius) >> 16);
        s.h = e.handle;
        statics_.push_back(s);
    };
    // [orig: pass 1 = itemDef type == Building; pass 2 = everything else with a
    // def — our instance map plays the "has a collision model" role.]
    // --- pool-0 persons + pool-1 dynamics. [orig: 0x4b9340] ---
    persons_.clear();
    dynamics_.clear();
    auto push_person = [&](const Entity &e) {
        if (e.kind != EntityKind::Organic || (e.flags & 1u) != 0) return;
        PersonSlot p;
        int32_t pf[3];
        entity_pos_fixed(e, pf);
        p.x = pf[0]; p.y = pf[1]; p.z = pf[2];
        p.radius = 0x10000; // [orig: entity boundRadius; person capsule ~1u] (D-COL-3)
        // A published live pose can place bones far from the feet (seated
        // poses, corpse spreads, the off-body pose regression test). Widen
        // the SLOT to the pose's own bone-sphere reach so the projectile
        // slot gate never excludes a posed bone; unposed persons keep the
        // capsule radius.
        auto inst = instances_.find(e.handle.packed);
        if (inst != instances_.end() && !inst->second.section_matrices.empty()) {
            const CollisionModel *m = model(inst->second.model_id);
            if (m != nullptr &&
                inst->second.section_matrices.size() == m->sections.size()) {
                int32_t max_reach = 0;
                for (size_t si = 0; si < m->sections.size(); ++si) {
                    const CollisionSection &sec = m->sections[si];
                    if (sec.radius < 0) continue;
                    int32_t c[3];
                    inst->second.section_matrices[si].transform_point(sec.center, c);
                    const int32_t reach =
                        vec_len_ftol(c[0] - pf[0], c[1] - pf[1], c[2] - pf[2]) +
                        std::max(sec.radius, 0xCCC);
                    if (reach > max_reach) max_reach = reach;
                }
                if (max_reach > p.radius) p.radius = max_reach;
            }
        }
        p.h = e.handle;
        persons_.push_back(p);
    };
    // Dynamics = mounted-object pool entities with instances that are NOT
    // buildings. Seat/armory carriers also belong in the proximity table when
    // their visual has no collision hull: attach scans consume this same slice,
    // and the old whole-registry fallback must not be their only discovery path.
    auto push_dynamic = [&](const Entity &e) {
        if (e.kind != EntityKind::Item || (e.flags & 1u) != 0) return;
        auto it = instances_.find(e.handle.packed);
        const CollisionModel *attached =
            it != instances_.end() ? model(it->second.model_id) : nullptr;
        const bool attachable = !e.seats.empty() || !e.armory_points.empty();
        if (attached == nullptr && e.bound_radius <= 0.0f && !attachable) return;
        DynSlot d;
        int32_t pf[3];
        entity_pos_fixed(e, pf);
        d.x = pf[0]; d.y = pf[1]; d.z = pf[2];
        d.radius = entity_proximity_radius(*this, e, attached);
        // Without a model/entity bound, retain a conservative sphere that
        // contains every authored interaction point. Rotation preserves this
        // local-space length; the source's +4u slice pad then covers the attach
        // gate around the point itself.
        if (attached == nullptr && e.bound_radius <= 0.0f) {
            auto include_point = [&](const Vec3 &p) {
                const double len = std::sqrt(
                        static_cast<double>(p.x) * p.x +
                        static_cast<double>(p.y) * p.y +
                        static_cast<double>(p.z) * p.z);
                d.radius = std::max(d.radius, to_fixed(len));
            };
            for (const Seat &seat : e.seats) include_point(seat.seat_local);
            for (const Vec3 &point : e.armory_points) include_point(point);
        }
        d.h = e.handle;
        dynamics_.push_back(d);
    };

    // Each output table retains the same registry-relative order, but their
    // independent filters share one capacity walk. Statics still require a
    // second pass so the complete Building prefix precedes every other pool-2
    // entry exactly as retail authored it.
    world.registry.for_each([&](const Entity &e) {
        if (e.handle.pool() == 2 && e.kind == EntityKind::Building) push_static(e);
        push_person(e);
        push_dynamic(e);
    });
    static_building_count_ = static_cast<int32_t>(statics_.size());
    world.registry.for_each([&](const Entity &e) {
        if (e.handle.pool() != 2 || e.kind == EntityKind::Building) return;
        push_static(e);
    });
    static_count_ = static_cast<int32_t>(statics_.size());

    if (!advance_candidate_slices) return;

    // --- per-entity candidate slices, every 17th tick. [orig: 0x4b8eb0 — pool 0
    // radius +4.0u, pool 1 +6.0u, shared 3000-entry arena; the call is gated on
    // dword_B57C84 >= 0x10 @ 0x4c240f (incremented per tick, zeroed inside the
    // builder), so slices are up to 16 ticks stale by design. Pool-1 SOURCE
    // slices (dynamics, +6.0u, the foliage-attrib parent gate) ride the vehicle
    // pass — only organics run our resolver today.] ---
    if (slice_refresh_counter_ < 16) {
        ++slice_refresh_counter_;
        return; // keep the previous slices/arena
    }
    slice_refresh_counter_ = 0;
    candidate_slices_built_ = true;
    arena_.clear();
    candidates_.clear();
    auto build_for = [&](const Entity &e, int32_t pad) {
        int32_t p[3];
        entity_pos_fixed(e, p);
        const int32_t source_radius =
                e.bound_radius > 0.0f ? to_fixed(e.bound_radius) : 0x10000;
        const int32_t range = source_radius + pad;
        CandidateSlice slice;
        slice.start = static_cast<int32_t>(arena_.size());
        slice.count = 0;
        for (const DynSlot &d : dynamics_) {
            if (d.h == e.handle) continue;
            const int32_t total = range + d.radius;
            if (abs32(d.x - p[0]) > total || abs32(d.y - p[1]) > total ||
                abs32(d.z - p[2]) > total)
                continue;
            if (vec_len_ftol(d.x - p[0], d.y - p[1], d.z - p[2]) > total) continue;
            if (arena_.size() >= 3000) break;
            arena_.push_back(d.h);
            ++slice.count;
        }
        for (int32_t i = 0; i < static_cast<int32_t>(statics_.size()); ++i) {
            const StaticSlot &s = statics_[i];
            if (s.h == e.handle) continue;
            const int32_t sx = static_slot_coord_q16(s.x);
            const int32_t sy = static_slot_coord_q16(s.y);
            const int32_t sz = static_slot_coord_q16(s.z);
            // No quantization slack: the original accepts the +-0.5u table error
            // as-is (the +111876 radius pad at table build absorbs it).
            // [orig: total = range + (radius << 16) @ 0x4b902f]
            const int32_t total = range + (static_cast<int32_t>(s.radius) << 16);
            if (abs32(sx - p[0]) > total || abs32(sy - p[1]) > total || abs32(sz - p[2]) > total)
                continue;
            if (vec_len_ftol(sx - p[0], sy - p[1], sz - p[2]) > total) continue;
            if (arena_.size() >= 3000) break;
            arena_.push_back(s.h);
            ++slice.count;
        }
        candidates_[e.handle.packed] = slice;
    };
    // persons_ uses the same registry order and exact organic/active predicate,
    // so it is also the already-compacted source walk for this refresh.
    for (const PersonSlot &person : persons_) {
        const Entity *e = world.registry.get(person.h);
        if (e != nullptr) build_for(*e, 0x40000); // [orig: pool-0 +4.0u]
    }
    // Pool-1 SOURCE slices for motor-driven vehicles (the hull contact query's
    // candidate set). [orig: Entity_BuildProximityListsFromPools @ 0x4b8eb0 — the
    // pool-1 leg, radius +6.0u @ 0x4b902f]
    world.registry.for_each([&](const Entity &e) {
        if (e.handle.pool() != 1 || (e.flags & 1u) != 0) return;
        const VehicleTraits *vt = world.vehicle_traits.get(e.item_id);
        if (vt == nullptr || vt->physics == 0) return;
        build_for(e, 0x60000);
    });
}

void CollisionWorld::refresh_after_registry_change(World &world) {
    // Clear even when no table has been published yet: refresh is the public
    // registry-lifetime edge and may follow a direct headless trace.
    invalidate_trace_views();
    if (!candidate_slices_built_) {
        // A pre-logic pool snapshot is already authoritative, so keep it
        // coherent after spawn/restore without consuming one of the initial 16
        // sliceless logic ticks. Never-built headless worlds retain fallback.
        if (tick_tables_built_) build_initial_tables(world);
        return;
    }
    // Entity_BuildAllProximityLists is the spawn/teleport path in retail. Route
    // through the normal builder so pool snapshots and the shared arena remain
    // one coherent epoch; forcing the cadence gate also resets its counter.
    slice_refresh_counter_ = 16;
    build_tick_tables(world);
}

} // namespace opennova::world
