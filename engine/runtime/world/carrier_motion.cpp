#include "carrier_motion.h"
#include "entity.h"
#include "vehicle_motor.h"
#include <base/io/bam.h>
#include <base/io/fixed.h>
#include <cmath>

namespace opennova::world {
// The standalone parent-delta follow every throwable motor shares [orig:
// Entity_InterpolateFromParentDelta @ 0x4a8d60 — code calls from the nade
// @ 0x44443b / projectile @ 0x444e8b / clym @ 0x4475d1 / schl @ 0x44861f
// motors, and installed as the placed motor +452 by
// Entity_ConvertRoundToPlacedEntity @ 0x5455fd]. Translate by the parent's
// (live − savedLivePose) per-tick delta, then on any attitude delta full-Euler
// rotate the child's offset about the parent — un-rotate by the SAVED attitude
// with negated sines, re-rotate by the CURRENT attitude with positive sines
// (Q22 trig, <<8 +127 bias, no capsule bias — the org deck rides inline the
// same twins with the capsule term; netsim row_deck_ride is the ported
// sibling) — and finally yaw += dyaw with the pitch/roll delta pair rotated by
// the pre-add parent-vs-child relative yaw. The delta reads the parent's own
// saved channel (Entity::saved_live_* — retail +0x80..+0x94), no rider-side
// copy; a never-stamped parent (statics) reads as zero delta.
void follow_carrier_motion(const Entity &parent, CarrierMotionPose &p) {
	if (!parent.saved_live_valid)
		return;
	int32_t cur[3];
	int32_t cyaw, cpitch, croll;
	carrier_pose_fixed(parent, cur, cyaw, cpitch, croll);
	const int32_t dpx = io::bam_sub(cur[0], parent.saved_live_pos[0]);
	const int32_t dpy = io::bam_sub(cur[1], parent.saved_live_pos[1]);
	const int32_t dpz = io::bam_sub(cur[2], parent.saved_live_pos[2]);
	const int32_t dyaw = io::bam_sub(cyaw, parent.saved_live_yaw);
	const int32_t dpitch = io::bam_sub(cpitch, parent.saved_live_pitch);
	const int32_t droll = io::bam_sub(croll, parent.saved_live_roll);
	// Translation delta [orig: @ 0x4a8e0f..0x4a8e18].
	p.pos[0] = io::bam_add(p.pos[0], dpx);
	p.pos[1] = io::bam_add(p.pos[1], dpy);
	p.pos[2] = io::bam_add(p.pos[2], dpz);
	if (dyaw == 0 && dpitch == 0 && droll == 0)
		return;
	const auto q22c = [](int32_t bam) {
		return static_cast<int32_t>(
				std::cos(static_cast<double>(bam) * 1.4629627251502471e-9) * io::kQ22One);
	};
	const auto q22s = [](int32_t bam) {
		return static_cast<int32_t>(
				std::sin(static_cast<double>(bam) * 1.4629627251502471e-9) * io::kQ22One);
	};
	const auto q22s_neg = [](int32_t bam) {
		return static_cast<int32_t>(
				std::sin(static_cast<double>(bam) * 1.4629627251502471e-9) * -io::kQ22One);
	};
	const auto m22 = [](int32_t a, int32_t b) {
		return static_cast<int32_t>((static_cast<int64_t>(a) * b) >> 22);
	};
	// Retail's SHLs wrap in 32-bit registers; shift through unsigned so a
	// negative offset is not C++ UB.
	const auto shl8 = [](int32_t v) { return static_cast<int32_t>(static_cast<uint32_t>(v) << 8); };
	const int32_t rel_x = shl8(p.pos[0] - cur[0]) + 127;
	const int32_t rel_y = shl8(p.pos[1] - cur[1]) + 127;
	const int32_t rel_z = shl8(p.pos[2] - cur[2]) + 127;
	// Un-rotate by the saved attitude (negated sines = the inverse)
	// [orig: @ 0x4a8e33..0x4a8fa0, dbl_7C57B0 = -4194304.0].
	const int32_t cys = q22c(parent.saved_live_yaw), sys = q22s_neg(parent.saved_live_yaw);
	const int32_t cps = q22c(parent.saved_live_pitch), sps = q22s_neg(parent.saved_live_pitch);
	const int32_t crs = q22c(parent.saved_live_roll), srs = q22s_neg(parent.saved_live_roll);
	const int32_t rot_yaw_x = m22(rel_x, cys) - m22(rel_y, sys);
	const int32_t rot_yaw_y = m22(rel_x, sys) + m22(rel_y, cys);
	const int32_t rot_pitch_x = m22(rot_yaw_x, cps);
	const int32_t rot_pitch_cross = m22(rel_z, sps);
	const int32_t rot_pitch_z = m22(rot_yaw_x, sps) + m22(rel_z, cps);
	const int32_t rot_roll_x = m22(rot_yaw_y, crs) - m22(rot_pitch_z, srs);
	const int32_t rot_roll_z = m22(rot_yaw_y, srs) + m22(rot_pitch_z, crs);
	const int32_t unrot_xz = rot_pitch_x - rot_pitch_cross;
	// Re-rotate by the current attitude (positive sines), roll -> pitch -> yaw
	// [orig: @ 0x4a8fab..0x4a90fb].
	const int32_t cyc = q22c(cyaw), syc = q22s(cyaw);
	const int32_t cpc = q22c(cpitch), spc = q22s(cpitch);
	const int32_t crc = q22c(croll), src = q22s(croll);
	const int32_t a = m22(rot_roll_x, src) + m22(rot_roll_z, crc);
	const int32_t b = m22(rot_roll_x, crc) - m22(rot_roll_z, src);
	const int32_t pp = m22(unrot_xz, cpc) - m22(a, spc);
	const int32_t zp = m22(unrot_xz, spc) + m22(a, cpc);
	p.pos[0] = cur[0] + ((m22(pp, cyc) - m22(b, syc)) >> 8);
	p.pos[1] = cur[1] + ((m22(pp, syc) + m22(b, cyc)) >> 8);
	p.pos[2] = cur[2] + (zp >> 8);
	// Attitude adoption: yaw += dyaw; the pitch/roll delta pair rotated by the
	// parent-vs-child relative yaw taken BEFORE the yaw add
	// [orig: @ 0x4a90fe..0x4a9170].
	const int32_t rel_yaw = io::bam_sub(cyaw, p.yaw_bam);
	const int32_t rc = q22c(rel_yaw), rs = q22s(rel_yaw);
	const int32_t pitch_d = m22(dpitch, rc) - m22(droll, rs);
	const int32_t roll_d = m22(dpitch, rs) + m22(droll, rc);
	p.yaw_bam = io::bam_add(p.yaw_bam, dyaw);
	p.pitch_bam = io::bam_add(p.pitch_bam, pitch_d);
	p.roll_bam = io::bam_add(p.roll_bam, roll_d);
}

} // namespace opennova::world
