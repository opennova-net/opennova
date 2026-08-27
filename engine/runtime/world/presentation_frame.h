#pragma once

// The ONE map between the engine's mission frame and the presentation frame
// the scene renders in. Mission space is the original's: x east, y north,
// z up (Z-up, the terrain plane is x/y). The presentation frame is a
// right-handed Y-up frame whose forward is -z: mission (x, y, z) ->
// (x, z, -y), so a mission facing yaw (sin yaw, cos yaw) faces
// (sin yaw, 0, -cos yaw) and the ground plane stays x/z.
//
// Every presenter, binding getter and probe converts through these, never by
// hand — the same map the present remap applies to entity rows.
// [orig: the view basis Camera_ComputeThirdPersonView @0x437d10 builds from
//  the mission-euler angles; the aim ray's far point Entity_BuildCameraView
//  (1000.0 q16 forward) @0x592910..0x59293c; the crosshair transform
//  HUD_DrawCrosshair @0x592a0f]

#include <cmath>
#include <cstdint>

namespace opennova::world {

// mission (x, y, z) -> presentation (x, z, -y).
inline void presentation_from_mission(const float m[3], float out[3]) {
    out[0] = m[0];
    out[1] = m[2];
    out[2] = -m[1];
}

// presentation (x, y, z) -> mission (x, -z, y): the inverse.
inline void mission_from_presentation(const float p[3], float out[3]) {
    out[0] = p[0];
    out[1] = -p[2];
    out[2] = p[1];
}

// The unit forward of mission-euler view angles (degrees) in the presentation
// frame: yaw around the up axis (0 = mission north = -z), pitch tilting it —
// (sin yaw cos pitch, sin pitch, -cos yaw cos pitch).
inline void presentation_forward_from_angles(float yaw_deg, float pitch_deg, float out[3]) {
    const double yr = static_cast<double>(yaw_deg) * (3.14159265358979323846 / 180.0);
    const double pr = static_cast<double>(pitch_deg) * (3.14159265358979323846 / 180.0);
    out[0] = static_cast<float>(std::sin(yr) * std::cos(pr));
    out[1] = static_cast<float>(std::sin(pr));
    out[2] = static_cast<float>(-std::cos(yr) * std::cos(pr));
}

// The FP camera roll's sign in the presentation frame: lean right (positive
// lean, positive torso roll) tilts the view right, which is a rotation about
// the camera's -forward (+z) axis by +roll — the presenter rotates about
// (0, 0, -1) by the negated angle. [orig: the on-foot person leg @0x437fe6]
inline float presentation_roll_rad(float roll_deg) {
    return static_cast<float>(static_cast<double>(roll_deg) * (3.14159265358979323846 / 180.0));
}

// The aim ray's far point: the eye plus kAimProjectRange along the view
// forward (presentation frame) [orig: Entity_BuildCameraView(entity, 1, 1,
// 65536000 = 1000.0 q16) @0x592910].
inline void aim_ray_endpoint(const float eye[3], float yaw_deg, float pitch_deg,
                             float range, float out[3]) {
    float fwd[3];
    presentation_forward_from_angles(yaw_deg, pitch_deg, fwd);
    out[0] = eye[0] + fwd[0] * range;
    out[1] = eye[1] + fwd[1] * range;
    out[2] = eye[2] + fwd[2] * range;
}

// The binocular rangefinder readout: the distance from the entity position to
// the ray endpoint (the collision hit, else the far point), truncated to an
// integer and clamped to the 1..1000 display range.
// [orig: the binocular range display clamp beside the aim ray]
inline constexpr int32_t kRangefinderMin = 1;
inline constexpr int32_t kRangefinderMax = 1000;
inline int32_t rangefinder_units(const float position[3], const float endpoint[3]) {
    const double dx = static_cast<double>(endpoint[0]) - position[0];
    const double dy = static_cast<double>(endpoint[1]) - position[1];
    const double dz = static_cast<double>(endpoint[2]) - position[2];
    const int32_t units = static_cast<int32_t>(std::sqrt(dx * dx + dy * dy + dz * dz));
    if (units < kRangefinderMin) return kRangefinderMin;
    if (units > kRangefinderMax) return kRangefinderMax;
    return units;
}

} // namespace opennova::world
