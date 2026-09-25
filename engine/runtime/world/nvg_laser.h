// The NVG IR laser beams: while the local player's night vision is on, every
// visible person holding a LaserBeam weapon shows its beam — a short ribbon
// from the weapon's action point along its authored direction, clipped by the
// first static or vehicle it meets, drawn through the tracer pool's immediate
// beam path in the tracer NVG style.
//
// [orig: sub_5C63B0 @ 0x5c63b0 (the walk over the visible-person list
//  dword_2984890, count dword_2984888) from Terrain_RenderSceneWithReflection
//  @ 0x5c9695 -> Entity_RenderNVGLaserBeam @ 0x5c6090 ->
//  Render_DrawTrailOrBeamSegments @ 0x5dcb80 (style 8, the NVG laser block
//  g_TracerStyle_NVGLaser); the draw is renderer::append_tracer_beam and the
//  overlay slot renderer::SceneOverlaySlot::NvgLaserBeams]
#pragma once

#include <cstdint>

#include <runtime/world/entity.h>

namespace opennova::world {

class CollisionWorld;
class World;

// The tracer style the beam draws with [orig: `push 8` @ 0x5c6389].
inline constexpr int kNvgLaserTracerStyle = 8;
// The ray length, 8.0 units [orig: 0x80000 @ 0x5c616b, the end point
// (dir << 5) >> 2 @ 0x5c6110..0x5c6131].
inline constexpr int32_t kNvgLaserRangeQ16 = 0x80000;
// The sample step, 0.25 units (i << 14 against the hit distance, the point
// offset (i * dir) >> 2) [orig: @ 0x5c6248..0x5c6286].
inline constexpr int32_t kNvgLaserStepQ16 = 0x4000;
// At most 32 samples, then the clip point twice [orig: `cmp ecx,20h`
// @ 0x5c6291; @ 0x5c629d..0x5c637e].
inline constexpr int kNvgLaserMaxSamples = 32;
inline constexpr int kNvgLaserMaxPoints = kNvgLaserMaxSamples + 2;

// The draw gate of one person: not seat-mounted (entity+0x157, the attach
// bone, zero), a held weapon definition (entity+0x298) whose flags carry
// LaserBeam (def+8 & 0x40000000), the local player's night vision on
// (g_NVGActive), the first-person camera (g_camera_mode 0), and not the local
// player itself. [orig: Entity_RenderNVGLaserBeam @ 0x5c609a..0x5c60e6]
struct NvgLaserGate {
	uint8_t attach_bone = 0;
	bool has_weapon_def = false;
	int32_t weapon_flags = 0;
	bool nvg_active = false;
	int camera_mode = 0;
	bool local_player = false;
};
bool nvg_laser_beam_drawn(const NvgLaserGate &gate);

// The beam's clip distance along the ray (Q16 units): the nearest static
// (pool 2) then vehicle (pool 1) hit short of the 8-unit range, else the
// range. `direction_q16` is the action point's authored direction (Q16).
// [orig: Projectile_RaycastProximitySlots(2, ray, 1) @ 0x5c61ef, then (1, ray,
//  1) @ 0x5c6218; each replaces only a nearer hit @ 0x5c61f7..0x5c622f]
int32_t nvg_laser_clip_distance(const CollisionWorld &collision, const World &world,
		EntityHandle person, const int32_t origin_q16[3], const int32_t direction_q16[3]);

// The beam's point run: a sample every 0.25 units while short of the clip
// distance (at most 32), then the clip point twice when the samples stopped
// short. Points are {x, y, z, w} in mission units, w = 1.0 (the width
// multiplier the ribbon build reads). Returns the point count.
// [orig: Entity_RenderNVGLaserBeam @ 0x5c6233..0x5c637e]
int nvg_laser_beam_points(const int32_t origin_q16[3], const int32_t direction_q16[3],
		int32_t clip_q16, float out_points[kNvgLaserMaxPoints * 4]);

} // namespace opennova::world
