#include <runtime/world/collision.h>
#include <runtime/world/pose_provider.h>

// CollisionWorld's model/instance registry and the per-tick proximity table build
// — what the queries above are pointed at.

#include <algorithm>
#include <cmath>

#include "collision_detail.h"

#include <runtime/world/world.h>

namespace opennova::world {

using namespace detail; // the shared fixed-point helpers, unqualified as before

namespace {

constexpr int64_t kStableLosCellSpanQ16 = int64_t{64} << 16;
constexpr uint64_t kStableLosMaxCandidateCells = 64;
constexpr uint64_t kStableLosMaxQueryCells = 4096;
constexpr uint64_t kStableLosMaxDenseCells = 65536;

int32_t stable_los_cell_coord(int64_t q16) {
    int64_t cell = q16 / kStableLosCellSpanQ16;
    if (q16 < 0 && q16 % kStableLosCellSpanQ16 != 0) --cell;
    return static_cast<int32_t>(cell);
}

uint64_t stable_los_cell_key(int32_t x, int32_t y) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) |
           static_cast<uint32_t>(y);
}

} // namespace

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
    if (h.pool() == 2) statics_dirty_ = true;
    invalidate_trace_view(h);
}

void CollisionWorld::remove_entity_instance(EntityHandle h) {
    if (!h.valid()) return;
    instances_.erase(h.packed);
    if (h.pool() == 2) statics_dirty_ = true;
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

void CollisionWorld::set_pose_provider(IPoseProvider *provider) {
    if (pose_provider_ == provider) return;
    pose_provider_ = provider;
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

int32_t CollisionWorld::entity_model_id(EntityHandle h) const {
    const auto it = instances_.find(h.packed);
    return it == instances_.end() ? -1 : it->second.model_id;
}

int32_t CollisionWorld::entity_husk_model_id(EntityHandle h) const {
    const auto it = instances_.find(h.packed);
    return it == instances_.end() ? -1 : it->second.husk_model_id;
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

const EntityHandle *CollisionWorld::wire_candidate_slice(
        uint16_t wire_handle, int32_t &count_out) const {
    const auto it = wire_candidates_.find(wire_handle);
    if (it == wire_candidates_.end() || it->second.count <= 0) {
        count_out = 0;
        return nullptr;
    }
    count_out = it->second.count;
    return wire_arena_.data() + it->second.start;
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

const CollisionModel *CollisionWorld::husk_model_for(
        const World &world, EntityHandle h) const {
    const Instance *instance = live_instance(world, h);
    if (instance == nullptr || instance->husk_model_id < 0) return nullptr;
    return model(instance->husk_model_id);
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
        if (h.pool() == 2) statics_dirty_ = true;
        invalidate_trace_view(h);
    }
    if (pose_provider_ == nullptr) return false;
    if (!pose_provider_->ensure_collision_instance(world, h)) return false;
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

void CollisionWorld::replace_wire_collision_proxies(
        std::vector<WirePersonCollisionProxy> persons,
        std::vector<WireDynamicCollisionProxy> dynamics,
        uint16_t local_player_wire_handle) {
    persons.erase(
        std::remove_if(persons.begin(), persons.end(),
                       [](const WirePersonCollisionProxy &proxy) {
                           return proxy.wire_handle == EntityHandle::kInvalid;
                       }),
        persons.end());
    std::stable_sort(
        persons.begin(), persons.end(),
        [](const WirePersonCollisionProxy &a,
           const WirePersonCollisionProxy &b) {
            return a.wire_handle < b.wire_handle;
        });
    dynamics.erase(
        std::remove_if(dynamics.begin(), dynamics.end(),
                       [](const WireDynamicCollisionProxy &proxy) {
                           return proxy.wire_handle == EntityHandle::kInvalid;
                       }),
        dynamics.end());
    std::stable_sort(
        dynamics.begin(), dynamics.end(),
        [](const WireDynamicCollisionProxy &a,
           const WireDynamicCollisionProxy &b) {
            return a.wire_handle < b.wire_handle;
        });
    wire_person_proxies_ = std::move(persons);
    wire_dynamic_proxies_ = std::move(dynamics);
    wire_local_player_handle_ = local_player_wire_handle;
}

void CollisionWorld::set_trace_profile_enabled(bool enabled) {
    if (trace_profile_enabled_ == enabled) return;
    trace_profile_enabled_ = enabled;
    trace_profile_ = TraceProfile{};
}

// --- ray-debug capture (the F3 ray view/window feed) ------------------------
// Dev tooling, not a ported surface: the segment-query seams record their
// inputs and results into these per-category rings while enabled; the queries
// themselves stay byte-identical.

const char *CollisionWorld::ray_debug_category_name(RayDebugCategory category) {
    switch (category) {
    case RayDebugCategory::kUncategorized: return "Uncategorized";
    case RayDebugCategory::kProjectile: return "Projectile";
    case RayDebugCategory::kKnife: return "Knife";
    case RayDebugCategory::kThrowable: return "Throwable";
    case RayDebugCategory::kAiLos: return "AI LOS";
    case RayDebugCategory::kReplicationLos: return "Replication LOS";
    case RayDebugCategory::kScriptLos: return "Script LOS";
    case RayDebugCategory::kExplosionLos: return "Explosion LOS";
    case RayDebugCategory::kGroundProbe: return "Ground probe";
    case RayDebugCategory::kCameraIris: return "Camera iris";
    case RayDebugCategory::kRenderOcclusion: return "Render occlusion";
    case RayDebugCategory::kSunVisibility: return "Sun visibility";
    case RayDebugCategory::kSoundOcclusion: return "Sound occlusion";
    case RayDebugCategory::kPrecipitation: return "Precipitation";
    case RayDebugCategory::kPick: return "Pick";
    case RayDebugCategory::kCount: break;
    }
    return "?";
}

void CollisionWorld::set_ray_debug_enabled(bool enabled) {
    if (ray_debug_enabled_ == enabled) return;
    ray_debug_enabled_ = enabled;
    for (RayDebugRing &ring : ray_debug_rings_) {
        ring.next = 0;
        ring.count = 0;
        ring.total = 0;
        if (enabled) {
            ring.events.assign(kRayDebugCapPerCategory, RayDebugEvent{});
        } else {
            // Free on disable: hosts/tests stack-allocate Worlds, keep the
            // ~165 KB of rings off that footprint (the RoundSim rationale).
            ring.events.clear();
            ring.events.shrink_to_fit();
        }
    }
}

void CollisionWorld::ray_debug_record(RayDebugCategory fallback, uint32_t tick,
                                      const int32_t a[3], const int32_t b[3],
                                      const int32_t *hit_or_null,
                                      uint8_t result) const {
    if (!ray_debug_enabled_) return;
    const RayDebugCategory category =
            ray_debug_scope_ != RayDebugCategory::kUncategorized ? ray_debug_scope_
                                                                 : fallback;
    RayDebugRing &ring = ray_debug_rings_[static_cast<size_t>(category)];
    if (ring.events.empty()) return; // enable raced a mid-flight query
    RayDebugEvent &event = ring.events[static_cast<size_t>(ring.next)];
    event.start = FixedVec3{a[0], a[1], a[2]};
    event.end = FixedVec3{b[0], b[1], b[2]};
    if (hit_or_null != nullptr) {
        event.hit = FixedVec3{hit_or_null[0], hit_or_null[1], hit_or_null[2]};
    } else {
        event.hit = event.end;
    }
    event.tick = tick;
    event.category = static_cast<uint8_t>(category);
    event.result = result;
    ring.next = (ring.next + 1) % kRayDebugCapPerCategory;
    if (ring.count < kRayDebugCapPerCategory) ++ring.count;
    ++ring.total;
}

const char *CollisionWorld::contact_debug_kind_name(ContactDebugKind kind) {
    switch (kind) {
    case ContactDebugKind::kProjectileHit: return "Projectile hit";
    case ContactDebugKind::kKnifeHit: return "Knife hit";
    case ContactDebugKind::kMoveContact: return "Move contact";
    case ContactDebugKind::kVehicleHull: return "Vehicle hull";
    case ContactDebugKind::kTerrainHit: return "Terrain hit";
    case ContactDebugKind::kWaterHit: return "Water hit";
    case ContactDebugKind::kCount: break;
    }
    return "?";
}

void CollisionWorld::set_contact_debug_enabled(bool enabled) {
    if (contact_debug_enabled_ == enabled) return;
    contact_debug_enabled_ = enabled;
    contact_debug_ring_.next = 0;
    contact_debug_ring_.count = 0;
    contact_debug_ring_.total = 0;
    for (uint64_t &total : contact_debug_ring_.kind_totals) total = 0;
    if (enabled) {
        contact_debug_ring_.events.assign(kContactDebugCap, ContactDebugEvent{});
    } else {
        // Free on disable: the ray-capture footprint rationale.
        contact_debug_ring_.events.clear();
        contact_debug_ring_.events.shrink_to_fit();
    }
}

void CollisionWorld::contact_debug_record(ContactDebugKind kind, uint32_t tick,
                                          EntityHandle target, const int32_t pos[3],
                                          uint8_t hit_class) const {
    if (!contact_debug_enabled_) return;
    ContactDebugRing &ring = contact_debug_ring_;
    if (ring.events.empty()) return; // enable raced a mid-flight query
    ContactDebugEvent &event = ring.events[static_cast<size_t>(ring.next)];
    event.pos = FixedVec3{pos[0], pos[1], pos[2]};
    event.tick = tick;
    event.target = target.valid() ? target.packed : 0xFFFF;
    event.kind = static_cast<uint8_t>(kind);
    event.hit_class = hit_class;
    ring.next = (ring.next + 1) % kContactDebugCap;
    if (ring.count < kContactDebugCap) ++ring.count;
    ++ring.total;
    ++ring.kind_totals[static_cast<size_t>(kind)];
}

void CollisionWorld::invalidate_trace_view(EntityHandle h) {
    if (h.valid()) trace_view_cache_.erase(h.packed);
    invalidate_stable_los_index();
}

void CollisionWorld::invalidate_trace_views() {
    trace_view_cache_.clear();
    invalidate_stable_los_index();
}

void CollisionWorld::invalidate_stable_los_index() {
    stable_los_index_ready_ = false;
    stable_los_candidates_.clear();
    stable_los_cell_spans_.clear();
    stable_los_large_candidates_.clear();
    stable_los_query_candidates_.clear();
    stable_los_query_marks_.clear();
    stable_los_query_generation_ = 0;
    stable_los_dense_enabled_ = false;
}

void CollisionWorld::reset_query_view_cache() {
    invalidate_trace_views();
}

void CollisionWorld::prepare_cached_raycast_queries(World &world) {
    devtools::ProfileLap lap(world.profile);
    // This is a new stable-world epoch: no matrix view or positional index may
    // survive from movement/destruction earlier in the logic tick.
    invalidate_trace_views();
    ++stable_los_index_epoch_;
    if (stable_los_index_epoch_ == 0) {
        // A wrap is practically unreachable, but stale cell epochs must never
        // alias the new publication.
        stable_los_cells_.clear();
        stable_los_dense_cells_.clear();
        stable_los_index_epoch_ = 1;
    }

    auto append = [&](const Entity &e) {
        if ((e.flags & 1u) != 0 || (e.engine_flags & 0x8000000u) != 0) return;
        StableLosCandidate candidate;
        candidate.h = e.handle;
        if (!target_bound(world, e.handle, candidate.pos, candidate.radius,
                          /*solid_only=*/true)) return;
        stable_los_candidates_.push_back(candidate);
    };

    if (tick_tables_ready()) {
        stable_los_candidates_.reserve(statics_.size() + dynamics_.size());
        // Every pool-2 entry, then every pool-1 entry, as raycast_clear_impl
        // walks them: the LOS reads the pools themselves, not the per-tick
        // proximity tables [orig: Physics_RaycastTerrainAndSectors
        // @0x539A16..0x539A30, @0x539A40..0x539A5A].
        world.registry.for_each_in_pool(2, append);
        world.registry.for_each_in_pool(1, append);
    } else {
        // Match raycast_clear_impl's unticked compatibility membership and
        // order exactly.
        world.registry.for_each([&](const Entity &e) {
            if (e.handle.pool() == 2) append(e);
        });
        world.registry.for_each([&](const Entity &e) {
            if (e.handle.pool() != 2 && e.kind == EntityKind::Item) append(e);
        });
    }

    lap.mark(devtools::Slot::SIM_REPLICATION_QUERY_COLLECT);

    // The grid publication as a whole (span + bucket + workspace) also lands
    // on the SIM_REPLICATION_QUERY_GRID row.
    const uint64_t grid_start = lap.last();
    stable_los_cell_spans_.reserve(stable_los_candidates_.size());
    bool have_cell_span = false;
    int32_t dense_min_x = 0;
    int32_t dense_max_x = -1;
    int32_t dense_min_y = 0;
    int32_t dense_max_y = -1;
    for (uint32_t index = 0;
         index < static_cast<uint32_t>(stable_los_candidates_.size()); ++index) {
        const StableLosCandidate &candidate = stable_los_candidates_[index];
        const int64_t radius = candidate.radius;
        const int32_t min_x = stable_los_cell_coord(
                static_cast<int64_t>(candidate.pos[0]) - radius);
        const int32_t max_x = stable_los_cell_coord(
                static_cast<int64_t>(candidate.pos[0]) + radius);
        const int32_t min_y = stable_los_cell_coord(
                static_cast<int64_t>(candidate.pos[1]) - radius);
        const int32_t max_y = stable_los_cell_coord(
                static_cast<int64_t>(candidate.pos[1]) + radius);
        const uint64_t width = static_cast<uint64_t>(
                static_cast<int64_t>(max_x) - min_x + 1);
        const uint64_t height = static_cast<uint64_t>(
                static_cast<int64_t>(max_y) - min_y + 1);
        if (width * height > kStableLosMaxCandidateCells) {
            // Very large authored bounds stay exact without exploding the
            // index: every query includes this deliberately tiny global set.
            stable_los_large_candidates_.push_back(index);
            continue;
        }
        stable_los_cell_spans_.push_back(
                {min_x, max_x, min_y, max_y, index});
        if (!have_cell_span) {
            dense_min_x = min_x;
            dense_max_x = max_x;
            dense_min_y = min_y;
            dense_max_y = max_y;
            have_cell_span = true;
        } else {
            dense_min_x = std::min(dense_min_x, min_x);
            dense_max_x = std::max(dense_max_x, max_x);
            dense_min_y = std::min(dense_min_y, min_y);
            dense_max_y = std::max(dense_max_y, max_y);
        }
    }
    lap.mark(devtools::Slot::SIM_REPLICATION_QUERY_GRID_SPAN);

    stable_los_dense_enabled_ = false;
    if (have_cell_span) {
        const uint64_t dense_width = static_cast<uint64_t>(
                static_cast<int64_t>(dense_max_x) - dense_min_x + 1);
        const uint64_t dense_height = static_cast<uint64_t>(
                static_cast<int64_t>(dense_max_y) - dense_min_y + 1);
        // Ordinary mission extents use direct indexing. Extremely sparse or
        // adversarial coordinates stay on the retained hash grid so memory is
        // bounded independently of world-coordinate range.
        stable_los_dense_enabled_ = dense_height != 0 &&
                dense_width <= kStableLosMaxDenseCells / dense_height;
        if (stable_los_dense_enabled_) {
            const uint64_t dense_area = dense_width * dense_height;
            stable_los_dense_min_x_ = dense_min_x;
            stable_los_dense_max_x_ = dense_max_x;
            stable_los_dense_min_y_ = dense_min_y;
            stable_los_dense_max_y_ = dense_max_y;
            stable_los_dense_height_ = static_cast<int32_t>(dense_height);
            if (stable_los_dense_cells_.size() < dense_area)
                stable_los_dense_cells_.resize(static_cast<size_t>(dense_area));
        }
    }
    if (stable_los_dense_enabled_) {
        for (const StableLosCellSpan &span : stable_los_cell_spans_) {
            for (int32_t x = span.min_x; x <= span.max_x; ++x) {
                const size_t row = static_cast<size_t>(
                        static_cast<int64_t>(x) - stable_los_dense_min_x_) *
                        static_cast<size_t>(stable_los_dense_height_);
                for (int32_t y = span.min_y; y <= span.max_y; ++y) {
                    StableLosCell &cell = stable_los_dense_cells_[
                            row + static_cast<size_t>(
                                    static_cast<int64_t>(y) - stable_los_dense_min_y_)];
                    if (cell.epoch != stable_los_index_epoch_) {
                        cell.candidates.clear();
                        cell.epoch = stable_los_index_epoch_;
                    }
                    cell.candidates.push_back(span.candidate);
                }
            }
        }
    } else {
        stable_los_cells_.reserve(stable_los_candidates_.size() * 2);
        for (const StableLosCellSpan &span : stable_los_cell_spans_) {
            for (int32_t x = span.min_x; x <= span.max_x; ++x) {
                for (int32_t y = span.min_y; y <= span.max_y; ++y) {
                    StableLosCell &cell =
                            stable_los_cells_[stable_los_cell_key(x, y)];
                    if (cell.epoch != stable_los_index_epoch_) {
                        cell.candidates.clear();
                        cell.epoch = stable_los_index_epoch_;
                    }
                    cell.candidates.push_back(span.candidate);
                }
            }
        }
    }
    lap.mark(devtools::Slot::SIM_REPLICATION_QUERY_GRID_BUCKET);
    stable_los_query_marks_.assign(stable_los_candidates_.size(), 0);
    stable_los_query_candidates_.reserve(stable_los_candidates_.size());
    stable_los_index_ready_ = true;
    lap.mark(devtools::Slot::SIM_REPLICATION_QUERY_GRID_WORKSPACE);
    if (lap.active())
        world.profile->add(devtools::Slot::SIM_REPLICATION_QUERY_GRID,
                           static_cast<int64_t>(lap.last() - grid_start));
}

const std::vector<uint32_t> &CollisionWorld::stable_los_candidates_for_ray(
        const CollisionRay &ray) {
    stable_los_query_candidates_.clear();
    if (!stable_los_index_ready_ || stable_los_candidates_.empty())
        return stable_los_query_candidates_;

    ++stable_los_query_generation_;
    if (stable_los_query_generation_ == 0) {
        std::fill(stable_los_query_marks_.begin(), stable_los_query_marks_.end(), 0);
        stable_los_query_generation_ = 1;
    }
    const uint32_t generation = stable_los_query_generation_;
    auto admit = [&](uint32_t index) {
        if (stable_los_query_marks_[index] == generation) return;
        stable_los_query_marks_[index] = generation;
        stable_los_query_candidates_.push_back(index);
    };

    for (uint32_t index : stable_los_large_candidates_) admit(index);

    const int32_t min_x = stable_los_cell_coord(
            std::min<int64_t>(ray.start[0], ray.end[0]));
    const int32_t max_x = stable_los_cell_coord(
            std::max<int64_t>(ray.start[0], ray.end[0]));
    const int32_t min_y = stable_los_cell_coord(
            std::min<int64_t>(ray.start[1], ray.end[1]));
    const int32_t max_y = stable_los_cell_coord(
            std::max<int64_t>(ray.start[1], ray.end[1]));
    const uint64_t width = static_cast<uint64_t>(
            static_cast<int64_t>(max_x) - min_x + 1);
    const uint64_t height = static_cast<uint64_t>(
            static_cast<int64_t>(max_y) - min_y + 1);
    if (width * height > kStableLosMaxQueryCells) {
        // A pathological map-spanning ray remains exact by falling back to
        // the prepared solid-only list.
        stable_los_query_candidates_.resize(stable_los_candidates_.size());
        for (uint32_t index = 0;
             index < static_cast<uint32_t>(stable_los_candidates_.size()); ++index)
            stable_los_query_candidates_[index] = index;
        return stable_los_query_candidates_;
    }

    if (stable_los_dense_enabled_) {
        const int32_t query_min_x = std::max(min_x, stable_los_dense_min_x_);
        const int32_t query_max_x = std::min(max_x, stable_los_dense_max_x_);
        const int32_t query_min_y = std::max(min_y, stable_los_dense_min_y_);
        const int32_t query_max_y = std::min(max_y, stable_los_dense_max_y_);
        for (int32_t x = query_min_x; x <= query_max_x; ++x) {
            const size_t row = static_cast<size_t>(
                    static_cast<int64_t>(x) - stable_los_dense_min_x_) *
                    static_cast<size_t>(stable_los_dense_height_);
            for (int32_t y = query_min_y; y <= query_max_y; ++y) {
                const StableLosCell &cell = stable_los_dense_cells_[
                        row + static_cast<size_t>(
                                static_cast<int64_t>(y) - stable_los_dense_min_y_)];
                if (cell.epoch != stable_los_index_epoch_) continue;
                for (uint32_t index : cell.candidates) admit(index);
            }
        }
    } else {
        for (int32_t x = min_x; x <= max_x; ++x) {
            for (int32_t y = min_y; y <= max_y; ++y) {
                const auto bucket = stable_los_cells_.find(stable_los_cell_key(x, y));
                if (bucket == stable_los_cells_.end() ||
                    bucket->second.epoch != stable_los_index_epoch_)
                    continue;
                for (uint32_t index : bucket->second.candidates) admit(index);
            }
        }
    }
    std::sort(stable_los_query_candidates_.begin(),
              stable_los_query_candidates_.end());
    return stable_los_query_candidates_;
}

void CollisionWorld::build_tick_tables(World &world) {
    if (trace_profile_enabled_) trace_profile_ = TraceProfile{};
    // Resolver contacts are a per-logic-tick stream. A pre-round or ended
    // match may deliberately skip the gameplay drain, so never carry a touch
    // into a later tick.
    change_team_contacts_.clear();
    movement_callback_contacts_.clear();
    build_tables(world, true);
}

std::vector<CollisionWorld::GameplayContact>
CollisionWorld::take_change_team_contacts() {
    std::vector<GameplayContact> contacts;
    contacts.swap(change_team_contacts_);
    return contacts;
}

std::vector<CollisionWorld::GameplayContact>
CollisionWorld::take_movement_callback_contacts() {
    std::vector<GameplayContact> contacts;
    contacts.swap(movement_callback_contacts_);
    return contacts;
}

void CollisionWorld::build_initial_tables(World &world) {
    // Mission load builds the FULL proximity state, candidate slices included —
    // retail's load path calls the slice builder directly, so the first logic
    // tick already grounds spawned entities on building floors instead of
    // letting them fall through during a sliceless boot window. The 17-tick
    // cadence governs steady-state REBUILDS only.
    // [orig: Game_TryLoadSavedGame -> Entity_BuildProximityListsFromPools
    //  @0x4b8eb0 (the builder resets g_ProxSliceRefreshCounter itself); the
    //  spawn/teleport paths (Entity_SpawnFromAnimSlotProperty,
    //  Entity_TeleportTeamToSpawn, EventAction_TeleportEntityToSpawn,
    //  HeliLift_SpawnPickup) also call it directly]
    slice_refresh_counter_ = 16; // force the cadence gate — retail's direct
                                 // builder call bypasses the @0x4c240f gate
    statics_dirty_ = true;
    build_tables(world, true);
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
            if (h.pool() == 2) statics_dirty_ = true;
            it = instances_.erase(it);
        } else {
            ++it;
        }
    }

    // --- pool-2 statics: buildings first, then the rest. Built at mission
    // start / teleport / the instance and registry edges that arm
    // statics_dirty_, never per tick [orig: Entity_BuildAllProximityLists
    // @0x4c20f0 -> 0x4b9430; xrefs = Entity_InitAllFromModels @0x40e5a1,
    // Game_StartMission @0x525c90 and the teleport paths] ---
    const bool rebuild_statics = statics_dirty_;
    if (rebuild_statics) {
        statics_.clear();
        static_building_count_ = 0;
    }
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
        auto inst = instances_.find(e.handle.packed);
        const CollisionModel *attached =
            inst != instances_.end() ? model(inst->second.model_id) : nullptr;
        p.radius = entity_proximity_radius(*this, e, attached);
        // A published live pose can place bones far from the feet (seated
        // poses, corpse spreads, the off-body pose regression test). Widen
        // the SLOT to the pose's own bone-sphere reach so the projectile
        // slot gate never excludes a posed bone; unposed persons keep the
        // capsule radius.
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
    // Retail also rejects dynamic CANDIDATES whose entity+0x114 carries ammo flag
    // `noage` (0x4000) when it builds the per-entity slices [orig:
    // Entity_BuildProximityListsFromPools @0x4b8fd4 (pool-0 sources) / @0x4b9214
    // (pool-1 sources); the pool-1 table build itself @0x4b9340 has no such test].
    // OpenNova's transient rounds live in RoundSim rather than the entity registry,
    // and round-to-placed-device conversion clears noage before the pool-1 entity
    // is materialized, so every registry item already satisfies that gate by
    // construction (a D-COL row for the omitted gate is proposed for sign-off).
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

    // Retail walks each pool's own array: pool 1 then pool 0 every tick
    // [orig: 0x4b9340 @0x4b9389 / @0x4b93eb], and pool 2 twice at the start
    // of a mission so the complete Building prefix precedes every other
    // pool-2 entry [orig: 0x4b9430].
    world.registry.for_each_in_pool(0, push_person);
    world.registry.for_each_in_pool(1, push_dynamic);
    if (rebuild_statics) {
        // The split key is the items.def TYPE, not the record family: every
        // pool-2 row is a BMS "building" record, but only the Building-type
        // defs form the prefix; the decorations and foliage follow it.
        // [orig: `def->type == ItemType_Building` @ 0x4b946e, `!=` @ 0x4b9502
        //  — building_def_row, world/collision.h]
        world.registry.for_each_in_pool(2, [&](const Entity &e) {
            if (building_def_row(e)) push_static(e);
        });
        static_building_count_ = static_cast<int32_t>(statics_.size());
        world.registry.for_each_in_pool(2, [&](const Entity &e) {
            if (!building_def_row(e)) push_static(e);
        });
        static_count_ = static_cast<int32_t>(statics_.size());
        statics_dirty_ = false;
    }

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
    build_candidate_slices(world);
}

void CollisionWorld::build_candidate_slices(World &world) {
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
    // Sources use their current live positions, including a direct teleport
    // between pool-table publications. Keep the existing organic source gate;
    // candidate target positions remain the published dynamics/statics.
    world.registry.for_each_in_pool(0, [&](const Entity &e) {
        if (e.kind == EntityKind::Organic && (e.flags & 1u) == 0)
            build_for(e, 0x40000); // [orig: pool-0 +4.0u]
    });
    // Pool-1 SOURCE slices for every retail-eligible active item, not merely
    // motor-driven vehicles. A source needs an ItemDef; EWeap/attachable rows
    // (attrib 0x20) are excluded unless the def's raw type is 1. This same
    // slice drives vehicle contact, iris/sun rays, sound, and other candidate
    // queries. [orig: Entity_BuildProximityListsFromPools @ 0x4b8eb0 — the
    // pool-1 loop head @ 0x4b9110: in-use word +0x1C @ 0x4b9127, ItemDef
    // +0x20 non-null @ 0x4b9130..0x4b9135, Flags bit 0 skip @ 0x4b9137..
    // 0x4b913b, def attrib 0x20 (+0x54) excluded unless def type (+0x5C)
    // == 1 @ 0x4b913d..0x4b9147; radius +6.0u (0x60000) @ 0x4b9152]
    world.registry.for_each([&](const Entity &e) {
        if (e.handle.pool() != 1 || (e.flags & 1u) != 0) return;
        if (!e.has_item_def) return;
        if ((e.item_attrib & kItemAttribEweap) != 0 && e.item_type != 1) return;
        build_for(e, 0x60000);
    });

    // Decoded rows occupy a distinct identity domain, but retail gives them
    // the same per-entity +0x1BC/+0x1C0 candidate pointer/count after the net
    // client has materialized its pools. Rebuild the wire arena on this SAME
    // 17-tick edge. Its entries remain registry collision targets because the
    // joiner hosts streamed pool-1/pool-2 models locally; only the SOURCE key
    // and pose are wire-owned. A verified registry_twin is the sole self-skip
    // path -- equal packed numbers alone do not alias the domains.
    wire_arena_.clear();
    wire_candidates_.clear();
    const auto build_wire_for = [&](uint16_t wire_handle,
                                    const FixedVec3 &position_q16,
                                    int32_t bound_radius_q16, int32_t pad,
                                    EntityHandle registry_twin) {
        const int32_t source_radius =
                bound_radius_q16 > 0 ? bound_radius_q16 : 0x10000;
        const int32_t range = source_radius + pad;
        CandidateSlice slice;
        slice.start = static_cast<int32_t>(wire_arena_.size());
        slice.count = 0;
        slice.built_pos[0] = position_q16.x;
        slice.built_pos[1] = position_q16.y;
        slice.built_pos[2] = position_q16.z;
        for (const DynSlot &d : dynamics_) {
            if (registry_twin.valid() && d.h == registry_twin) continue;
            const int32_t total = range + d.radius;
            if (abs32(d.x - position_q16.x) > total ||
                abs32(d.y - position_q16.y) > total ||
                abs32(d.z - position_q16.z) > total)
                continue;
            if (vec_len_ftol(d.x - position_q16.x,
                             d.y - position_q16.y,
                             d.z - position_q16.z) > total)
                continue;
            if (wire_arena_.size() >= 3000) break;
            wire_arena_.push_back(d.h);
            ++slice.count;
        }
        for (const StaticSlot &s : statics_) {
            if (registry_twin.valid() && s.h == registry_twin) continue;
            const int32_t sx = static_slot_coord_q16(s.x);
            const int32_t sy = static_slot_coord_q16(s.y);
            const int32_t sz = static_slot_coord_q16(s.z);
            const int32_t total =
                    range + (static_cast<int32_t>(s.radius) << 16);
            if (abs32(sx - position_q16.x) > total ||
                abs32(sy - position_q16.y) > total ||
                abs32(sz - position_q16.z) > total)
                continue;
            if (vec_len_ftol(sx - position_q16.x,
                             sy - position_q16.y,
                             sz - position_q16.z) > total)
                continue;
            if (wire_arena_.size() >= 3000) break;
            wire_arena_.push_back(s.h);
            ++slice.count;
        }
        wire_candidates_[wire_handle] = slice;
    };
    for (const WirePersonCollisionProxy &person : wire_person_proxies_) {
        build_wire_for(person.wire_handle, person.position_q16,
                       person.bound_radius_q16, 0x40000, EntityHandle{});
    }
    for (const WireDynamicCollisionProxy &dynamic : wire_dynamic_proxies_) {
        if (!dynamic.candidate_source_eligible) continue;
        build_wire_for(dynamic.wire_handle, dynamic.position_q16,
                       dynamic.bound_radius_q16, 0x60000,
                       dynamic.registry_twin);
    }
}

void CollisionWorld::refresh_entity_proximity(World &world, Entity &entity) {
    invalidate_trace_views();
    build_candidate_slices(world);
    refresh_blink(world, entity);
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
    // Entity_BuildAllProximityLists is the spawn/teleport path in retail: the
    // statics table rebuilds here and nowhere per tick. Route through the
    // normal builder so pool snapshots and the shared arena remain one coherent
    // epoch; forcing the cadence gate also resets its counter.
    slice_refresh_counter_ = 16;
    statics_dirty_ = true;
    build_tick_tables(world);
}

} // namespace opennova::world
