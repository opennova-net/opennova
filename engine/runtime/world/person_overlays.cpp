#include <runtime/world/person_overlays.h>

#include <base/io/bam.h>
#include <runtime/world/angle.h>
#include <runtime/world/entity.h>
#include <runtime/world/present_rows.h>

namespace opennova::world {

namespace {

float mission_pitch_or_roll_deg(int32_t bam) {
	return static_cast<float>(static_cast<double>(bam) * kDegreesPerBam);
}

} // namespace

// [orig: BoneCallback_org0_World @ 0x4e3940]
PersonOverlays person_overlays(const PersonOverlayInputs &in) {
	PersonOverlays out;
	// Draw 1: either canopy word nonzero draws the canopy; both words reach
	// the CTRL bus doubled. The turn is fed (-cos h, +sin h) of the body
	// heading h, the builder's (cos, -sin) form of h + pi.
	// [orig: gate @ 0x4e3988..0x4e399a; the axis-2 feed @ 0x4e39d6..0x4e3a0a
	//  (sin * dbl_7C3600 = +2^22, cos * dbl_7C57B0 = -2^22); PARA / PARA_O
	//  stores @ 0x4e3a16..0x4e3a56]
	if (in.chute.inflation != 0 || in.chute.flap != 0) {
		out.canopy_para = 2 * static_cast<int32_t>(in.chute.inflation);
		out.canopy_para_o = 2 * static_cast<int32_t>(in.chute.flap);
		out.canopy_yaw_deg = static_cast<float>(mission_yaw_deg_from_bam_heading(
				io::bam_add(in.pose.body_yaw, INT32_MIN)));
	}
	// Draw 3 on Flags & 4; its NVG_FLIP register follows the dead bit.
	// [orig: @ 0x4e3b4f..0x4e3b54; `and al, 2; neg al; sbb; and 0FFFFh`
	//  @ 0x4e3b90..0x4e3b9d]
	out.nvg = (in.flags & kEntityFlagNVGWorn) != 0;
	if (out.nvg) out.nvg_flip = (in.flags & kEntityFlagDead) != 0 ? 0xFFFF : 0;
	// Draw 4 needs the aim-branch anim state AND Flags & 8.
	// [orig: g_AnimStateFlagsTable[entity+0x2BC] & 0x40 @ 0x4e3bf2,
	//  Flags & 8 @ 0x4e3c00]
	out.binoculars = in.pose.aim_state && (in.flags & kEntityFlagBinoculars) != 0;
	// Draw 6: the mounted child with an item def, oriented by the carrier's
	// entity triple (the saved yaw/pitch/roll the bone build restored).
	// [orig: @ 0x4e3da6..0x4e3db7; Math_BuildFixedPointToFloatMatrix4x4(entity+4)
	//  @ 0x4e3dc6]
	if (in.carried_type_id != 0) {
		out.carried_type_id = in.carried_type_id;
		out.carried_pitch_deg = mission_pitch_or_roll_deg(in.pose.aim_pitch);
		out.carried_yaw_deg = static_cast<float>(
				mission_yaw_deg_from_bam_heading(in.pose.aim_yaw));
		out.carried_roll_deg = mission_pitch_or_roll_deg(in.pose.roll);
	}
	return out;
}

void write_present_person_overlays(float *row, const PersonOverlays &o) {
	row[PF_CANOPY_PARA] = static_cast<float>(o.canopy_para);
	row[PF_CANOPY_PARA_O] = static_cast<float>(o.canopy_para_o);
	row[PF_CANOPY_YAW_DEG] = o.canopy_yaw_deg;
	row[PF_NVG_WORN] = o.nvg ? 1.0f : 0.0f;
	row[PF_NVG_FLIP] = static_cast<float>(o.nvg_flip);
	row[PF_BINOCULARS_RAISED] = o.binoculars ? 1.0f : 0.0f;
	row[PF_CARRIED_TYPE_ID] = static_cast<float>(o.carried_type_id);
	row[PF_CARRIED_PITCH_DEG] = o.carried_pitch_deg;
	row[PF_CARRIED_YAW_DEG] = o.carried_yaw_deg;
	row[PF_CARRIED_ROLL_DEG] = o.carried_roll_deg;
}

PersonOverlays read_present_person_overlays(const float *row) {
	PersonOverlays o;
	o.canopy_para = static_cast<int32_t>(row[PF_CANOPY_PARA]);
	o.canopy_para_o = static_cast<int32_t>(row[PF_CANOPY_PARA_O]);
	o.canopy_yaw_deg = row[PF_CANOPY_YAW_DEG];
	o.nvg = row[PF_NVG_WORN] != 0.0f;
	o.nvg_flip = static_cast<int32_t>(row[PF_NVG_FLIP]);
	o.binoculars = row[PF_BINOCULARS_RAISED] != 0.0f;
	o.carried_type_id = static_cast<int32_t>(row[PF_CARRIED_TYPE_ID]);
	o.carried_pitch_deg = row[PF_CARRIED_PITCH_DEG];
	o.carried_yaw_deg = row[PF_CARRIED_YAW_DEG];
	o.carried_roll_deg = row[PF_CARRIED_ROLL_DEG];
	return o;
}

} // namespace opennova::world
