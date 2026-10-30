// The general integer-ray variant used by squib and by the sun glare's and water glint's
// entity rays, not the bullet face query.
// [orig: Entity_ProcessProjectileTravel @0x448D50, Physics_RaycastAgainstEntityPool @0x538720]
#include <runtime/world/collision.h>
#include <base/io/fixed.h>
#include <runtime/world/world.h>
#include <runtime/terrain_query/height_field.h>
#include "collision_detail.h"
#include <algorithm>

namespace opennova::world {
namespace {
// The walker's ray [orig: Physics_InitRayContext @0x5385E0]: the segment, its length
// truncated, the direction divided by that length; `distance` < 16 is a ray its callers take
// as clear (the context returns 1 and builds nothing).
CollisionRay int_context_ray(const FixedVec3 &start, const FixedVec3 &end, int32_t &distance) {
    using namespace detail;
    CollisionRay ray;
    ray.start[0]=start.x; ray.start[1]=start.y; ray.start[2]=start.z;
    ray.end[0]=end.x; ray.end[1]=end.y; ray.end[2]=end.z;
    ray.refresh();
    distance = vec_len_ftol(io::bam_sub(end.x,start.x),
            io::bam_sub(end.y,start.y),io::bam_sub(end.z,start.z));
    if (distance < 16) return ray;
    // IntContext divides 65536.0f by the truncated / clamped length first, then scales each
    // component by that factor [orig: `fdivr ds:flt_7C32BC` @0x53867D, the products
    // @0x538683..0x5386A2].
    const double k = 65536.0 / distance;
    for (int a=0;a<3;++a)
        ray.dir[a]=int32_t(double(io::bam_sub(ray.end[a],ray.start[a])) * k);
    return ray;
}
} // namespace

bool CollisionWorld::pool_row_hits_ray(const World &world, EntityHandle handle, const Entity &e,
        const CollisionRay &ray, EntityHandle excluded, const Entity *excluded_row) const {
    using namespace detail;
    if (!e.has_item_def || e.bound_radius.q16 == 0 || handle==excluded ||
            ((e.engine_flags|e.flags)&1u) ||
            (e.ground_target.valid() && e.ground_target==excluded)) return false;
    const int32_t p[]={e.position.x.q16,e.position.y.q16,
            e.position.z.q16};
    // The row's bound word (entity+0) [orig: Physics_RaycastAgainstEntityPool
    //  @0x538720 - `mov eax, [edx]; test eax, eax; jz` @0x53879A..0x53879E]
    const int32_t radius=e.bound_radius.q16;
    bool outside=false;
    for (int a=0;a<3;++a)
        outside |= std::min(ray.start[a],ray.end[a])>int64_t(p[a])+radius ||
                std::max(ray.start[a],ray.end[a])<int64_t(p[a])-radius;
    if (outside || ray_line_distance(ray.start,ray.dir,p)>radius) return false;
    if (e.item_type==3)
        return !(excluded_row && excluded_row->team==e.team &&
                vec_len_ftol(p[0]-ray.start[0],p[1]-ray.start[1],p[2]-ray.start[2])<196608);
    CollisionTargetView view;
    std::vector<CollisionMatrix> matrices;
    const auto *target=target_view(world,handle,view,matrices);
    CollisionRay solid=ray;
    return target && collision_raycast_model(*target,solid);
}

bool CollisionWorld::sun_ray_clear_of_entities(const World &world, const FixedVec3 &start,
        const FixedVec3 &end) const {
    int32_t distance=0;
    const CollisionRay ray=int_context_ray(start,end,distance);
    if (distance < 16) return true;
    const EntityHandle local=world.cached.local_player;
    const Entity *local_row=world.registry.get(local);
    // Pool 2, then 1, then 0, each to its used count; the first hit ends the test
    // [orig: Physics_RaycastAgainstPoolByIndex @0x539530 - the used load @0x539540].
    for (const int pool : {2,1,0})
        for (size_t i=0;i<world.registry.pool_used(pool);++i) {
            const auto handle=EntityHandle::make(pool,uint16_t(i));
            const Entity *e=world.registry.get(handle);
            if (e && pool_row_hits_ray(world,handle,*e,ray,local,local_row)) return false;
        }
    // A joiner's pools hold every decoded row: the movers the registry has no twin of (pool 1) and
    // the remote persons (pool 0) are the wire proxies, the row tests' gates over their live poses
    // (env #49) [orig: Render_SkyboxSunGlow @0x5aceae..0x5acede, Environment_UpdateSunGlare
    //  @0x5ad2cb..0x5ad2fb -> Physics_RaycastAgainstEntityPool @0x538720 over the client's pools].
    if (!world.rules.mp_session || world.rules.projectile_authority) return true;
    using namespace detail;
    const auto in_bound = [&ray](const FixedVec3 &position, int32_t radius) {
        const int32_t p[]={position.x,position.y,position.z};
        bool outside=false;
        for (int a=0;a<3;++a)
            outside |= std::min(ray.start[a],ray.end[a])>int64_t(p[a])+radius ||
                    std::max(ray.start[a],ray.end[a])<int64_t(p[a])-radius;
        return !outside && ray_line_distance(ray.start,ray.dir,p)<=radius;
    };
    for (const WireDynamicCollisionProxy &proxy : wire_dynamic_proxies_) {
        if (proxy.registry_twin.valid() || proxy.bound_radius_q16 == 0 || (proxy.live_flags & 1u)) continue;
        if (!in_bound(proxy.live_position_q16, proxy.bound_radius_q16)) continue;
        const CollisionModel *solid_model = model(proxy.model_id);
        if (solid_model == nullptr || !solid_model->valid()) continue;
        const int32_t pos[3]={proxy.live_position_q16.x,proxy.live_position_q16.y,proxy.live_position_q16.z};
        const CollisionMatrix placed = collision_matrix_from_placement(proxy.heading_bam, proxy.pitch_bam,
                proxy.roll_bam, pos, proxy.uniform_scale_q16);
        std::vector<CollisionMatrix> matrices(solid_model->sections.size(), placed);
        CollisionTargetView view;
        view.model = solid_model;
        view.matrices = matrices.data();
        view.pos[0]=pos[0];
        view.pos[1]=pos[1];
        view.pos[2]=pos[2];
        view.yaw_bam = proxy.heading_bam;
        view.pitch_bam = proxy.pitch_bam;
        view.bound_radius = proxy.bound_radius_q16;
        view.pool_index = proxy.wire_handle & 0xFFF;
        view.uniform_scale_q16 = proxy.uniform_scale_q16;
        view.live_section_pose = false;
        CollisionRay solid=ray;
        if (collision_raycast_model(view,solid)) return false;
    }
    for (const WirePersonCollisionProxy &proxy : wire_person_proxies_) {
        if (proxy.bound_radius_q16 == 0 || (proxy.live_flags & 1u)) continue;
        if (!in_bound(proxy.live_position_q16, proxy.bound_radius_q16)) continue;
        const FixedVec3 &p = proxy.live_position_q16;
        if (local_row && local_row->team==proxy.team &&
                vec_len_ftol(p.x-ray.start[0],p.y-ray.start[1],p.z-ray.start[2])<196608)
            continue;
        return false;
    }
    return true;
}

EntityHandle CollisionWorld::raycast_nearest_entity(const World &world, EntityHandle exclude,
        const FixedVec3 &start, const FixedVec3 &end) const {
    using namespace detail;
    int32_t length=0;
    const CollisionRay ray=int_context_ray(start,end,length);
    if (length < 16) return {}; // [orig: Physics_InitRayContext's 1 return @0x538668]
    int32_t lo[3], hi[3];
    for (int a=0;a<3;++a) {
        lo[a]=std::min(ray.start[a],ray.end[a]);
        hi[a]=std::max(ray.start[a],ray.end[a]);
    }
    // One pool's walk: the nearest face hit, its distance; 0 is no hit.
    // [orig: Physics_RaycastEntityPool @0x4143E0]
    const auto walk=[&](int pool, EntityHandle &hit) {
        int32_t best=length; // [orig: ctx+0x60 = ctx+0x24 @0x4143FF]
        bool any=false;
        CollisionTargetView view;
        std::vector<CollisionMatrix> matrices;
        // The pool to its used count [orig: @0x414419].
        for (size_t i=0;i<world.registry.pool_used(pool);++i) {
            const auto handle=EntityHandle::make(pool,uint16_t(i));
            const Entity *e=world.registry.get(handle);
            // [orig: the +0x1C and bound gates @0x41445C..0x41446E]
            if (e==nullptr || e->item_type_index==0 || e->bound_radius.q16==0) continue;
            const int32_t r=e->bound_radius.q16;
            const int32_t p[3]={e->position.x.q16,e->position.y.q16,e->position.z.q16};
            // The bound box, wrapping adds and signed compares [orig: @0x414470..0x4144A6].
            bool outside=false;
            for (int a=0;a<3;++a)
                outside|=lo[a]>io::bam_add(p[a],r) || hi[a]<io::bam_sub(p[a],r);
            if (outside || handle==exclude) continue; // [orig: the excludes @0x4144A8..0x4144BA]
            // [orig: the line distance @0x4144BC..0x4145C5]
            if (ray_line_distance(ray.start,ray.dir,p)>r) continue;
            // The intact model whatever the husk bit [orig: `mov esi, [ecx+30h]` @0x4145CF].
            const CollisionTargetView *target=target_view(world,handle,view,matrices,false);
            if (target==nullptr) continue; // [orig: the bone data's null @0x4145EF]
            if (collision_raycast_entity_faces(*target,ray.start,ray.end,ray.dir,length,best)) {
                hit=handle;
                any=true;
            }
        }
        return any ? best : 0; // [orig: @0x414968..0x414994]
    };
    // Pool 2's hit stands when nearer than the length; pool 1's when strictly nearer than that.
    // [orig: Physics_RaycastNearestEntity @0x5394B0 - pool 2 @0x5394ED, `jge` @0x5394FF, pool 1
    //  @0x53950E, `jge` @0x53951E]
    EntityHandle best_entity, hit;
    int32_t best_distance=length;
    const int32_t d2=walk(2,hit);
    if (d2!=0 && d2<best_distance) {
        best_entity=hit;
        best_distance=d2;
    }
    const int32_t d1=walk(1,hit);
    if (d1!=0 && d1<best_distance) return hit;
    return best_entity;
}

ProjectileHit CollisionWorld::trace_squib(World &world, EntityHandle source,
        const FixedVec3 &start, const FixedVec3 &end) {
    using namespace detail;
    int32_t distance=0;
    CollisionRay ray=int_context_ray(start,end,distance);
    ProjectileHit best;
    if (distance < 16) return best;
    const Entity *owner=world.registry.get(source);
    if (terrain && terrain->valid()) {
        int32_t hit[3];
        if (terrain_clip_segment(*terrain,ray.start,ray.end,hit)) {
            best.hit_class=ProjectileHitClass::Terrain;
            best.surface_type=terrain::surface_type_at_fixed(world.tables.surface_map,end.x,end.y);
            const int32_t hd=vec_len_ftol(hit[0]-start.x,hit[1]-start.y,hit[2]-start.z);
            const int32_t height=terrain::height_field_height_bilinear_fixed(*terrain,hit[0],hit[1]);
            if (distance > hd && hit[2] < height) {
                const int32_t dz=abs32(hit[2]-start.z);
                distance=dz>6553 ? int32_t((int64_t(int32_t(int64_t(
                        abs32(height-start.z))*65536/dz))*hd+(io::kFp16OneInt / 2))>>16) : 100;
            }
        }
    }
    const int32_t water=world.env.water_z;
    if (water && ((start.z>water && end.z<water) || (start.z<water && end.z>water)) &&
            ray.dir[2]) {
        // Both original crossing branches negate the quotient (including up).
        const int32_t wd=io::bam_sub(0, int32_t(int64_t(abs32(start.z-water))*65536/ray.dir[2]));
        if (wd<distance) { distance=wd; best.hit_class=ProjectileHitClass::Water; }
    }
    // A NoTarget row (Flags 0x8000000) stays in: each pool call passes the
    // walker's include arg 1 [orig: `push 1` @0x449112 / @0x44926B / @0x4493C0
    // -> Physics_RaycastAgainstPoolByIndex @0x539530 ->
    // Physics_RaycastAgainstEntityPool, the skip @0x5387A4..0x5387B4].
    for (const int pool : {0,2,1}) {
        ProjectileHit candidate;
        // The pool to its used count [orig: Physics_RaycastAgainstPoolByIndex @0x539540].
        for (size_t i=0;i<world.registry.pool_used(pool);++i) {
            const auto handle=EntityHandle::make(pool,uint16_t(i));
            const Entity *e=world.registry.get(handle);
            if (!e || !pool_row_hits_ray(world,handle,*e,ray,source,owner)) continue;
            const int32_t p[]={e->position.x.q16,e->position.y.q16,
                    e->position.z.q16};
            const int32_t projected=int32_t((int64_t(ray.dir[0])*(p[0]-start.x)+
                    int64_t(ray.dir[1])*(p[1]-start.y)+int64_t(ray.dir[2])*(p[2]-start.z))>>16);
            for (int a=0;a<3;++a)
                ray.end[a]=io::bam_add(ray.start[a],int32_t((int64_t(ray.dir[a])*projected+(io::kFp16OneInt / 2))>>16));
            ray.refresh_bounds();
            candidate.hit_class=pool==0 ? ProjectileHitClass::Person :
                    pool==2 ? ProjectileHitClass::StaticEntity : ProjectileHitClass::DynamicEntity;
            candidate.geometry_entity=handle;
            candidate.distance_q16=vec_len_ftol(ray.end[0]-start.x,ray.end[1]-start.y,ray.end[2]-start.z);
            // This walker leaves the IntContext's zero material/section fields.
            candidate.surface_type=0; candidate.bone_index=0; candidate.hit_zone=0;
            candidate.section_index=0;
        }
        if (candidate.hit() && (candidate.distance_q16<distance ||
                (pool==0 && candidate.distance_q16==distance))) {
            best=candidate; distance=candidate.distance_q16;
        }
    }
    best.distance_q16=distance;
    best.position_q16=end;
    return best;
}
} // namespace opennova::world
