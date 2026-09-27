// The general integer-ray variant used by squib, not the bullet face query.
// [orig: Entity_ProcessProjectileTravel @0x448D50, Physics_RaycastAgainstEntityPool @0x538720]
#include <runtime/world/collision.h>
#include <base/io/fixed.h>
#include <runtime/world/world.h>
#include <runtime/terrain_query/height_field.h>
#include "collision_detail.h"
#include <algorithm>

namespace opennova::world {
ProjectileHit CollisionWorld::trace_squib(World &world, EntityHandle source,
        const FixedVec3 &start, const FixedVec3 &end) {
    using namespace detail;
    CollisionRay ray;
    ray.start[0]=start.x; ray.start[1]=start.y; ray.start[2]=start.z;
    ray.end[0]=end.x; ray.end[1]=end.y; ray.end[2]=end.z;
    ray.refresh();
    int32_t distance = vec_len_ftol(io::bam_sub(end.x,start.x),
            io::bam_sub(end.y,start.y),io::bam_sub(end.z,start.z));
    ProjectileHit best;
    if (distance < 16) return best;
    // IntContext divides by the truncated/clamped length.
    for (int a=0;a<3;++a)
        ray.dir[a]=int32_t(double(io::bam_sub(ray.end[a],ray.start[a])) * 65536.0/distance);
    const Entity *owner=world.registry.get(source);
    if (terrain && terrain->valid()) {
        int32_t hit[3];
        if (terrain_clip_segment(*terrain,ray.start,ray.end,hit)) {
            best.hit_class=ProjectileHitClass::Terrain;
            best.surface_type=terrain::surface_type_at_fixed(world.tables.surface_map,end.x,end.y);
            const int32_t hd=vec_len_ftol(hit[0]-start.x,hit[1]-start.y,hit[2]-start.z);
            const int32_t height=int32_t(terrain::height_field_height_world_bilinear(
                    *terrain,hit[0]/65536.0f,-hit[1]/65536.0f)*65536);
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
    for (const int pool : {0,2,1}) {
        ProjectileHit candidate;
        for (size_t i=0;i<world.registry.pool_capacity(pool);++i) {
            const auto handle=EntityHandle::make(pool,uint16_t(i));
            const Entity *e=world.registry.get(handle);
            if (!e || !e->has_item_def || !e->bound_radius || handle==source ||
                    ((e->engine_flags|e->flags)&(1u|0x8000000u)) ||
                    (e->ground_target.valid() && e->ground_target==source)) continue;
            const int32_t p[]={int32_t(e->position.x*65536),int32_t(e->position.y*65536),
                    int32_t(e->position.z*65536)};
            const int32_t radius=int32_t(e->bound_radius*65536);
            bool outside=false;
            for (int a=0;a<3;++a)
                outside |= std::min(ray.start[a],ray.end[a])>int64_t(p[a])+radius ||
                        std::max(ray.start[a],ray.end[a])<int64_t(p[a])-radius;
            if (outside || ray_line_distance(ray.start,ray.dir,p)>radius) continue;
            if (e->item_type==3) {
                if (owner && owner->team==e->team &&
                        vec_len_ftol(p[0]-start.x,p[1]-start.y,p[2]-start.z)<196608) continue;
            } else {
                CollisionTargetView view;
                std::vector<CollisionMatrix> matrices;
                const auto *target=target_view(world,handle,view,matrices);
                CollisionRay solid=ray;
                if (!target || !collision_raycast_model(*target,solid)) continue;
            }
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
