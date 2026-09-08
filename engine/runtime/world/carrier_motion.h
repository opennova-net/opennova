#pragma once

#include <cstdint>

namespace opennova::world {
struct Entity;

// A carried position in Q16 and attitude in BAM32.
struct CarrierMotionPose {
	int32_t pos[3] = {};
	int32_t yaw_bam = 0;
	int32_t pitch_bam = 0;
	int32_t roll_bam = 0;
};

// Translate by the carrier delta, then unrotate the saved attitude and rotate
// the current attitude around its origin. There is no distance/drop policy in
// this operation. [orig: Entity_InterpolateFromParentDelta @ 0x4A8D60;
// vehicle inlines @ 0x48D6DA..0x48DACD / @ 0x4905BC..0x49095B]
void follow_carrier_motion(const Entity &carrier, CarrierMotionPose &pose);
} // namespace opennova::world
