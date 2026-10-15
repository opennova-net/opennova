#pragma once

// What a ragdoll collides with, asked in the presentation frame (Y up, the
// frame the bodies simulate in). Every query is a read: the ragdoll never
// touches the world it asks about.

#include <runtime/ragdoll/ragdoll_math.h>

namespace opennova::ragdoll {

class Collider {
public:
	virtual ~Collider() = default;
	// The ground's height and unit normal under (x, z); false where there is
	// no ground to stand on.
	virtual bool ground(float x, float z, float &r_height, Vec3 &r_normal) const = 0;
	// The first solid (static geometry, not the ground) the segment a -> b
	// meets: its point and unit normal facing back along the segment.
	virtual bool sweep(const Vec3 &a, const Vec3 &b, Vec3 &r_point, Vec3 &r_normal) const = 0;
};

} // namespace opennova::ragdoll
