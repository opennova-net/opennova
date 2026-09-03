// The held-weapon attach math on the retail data. The presentation record the
// replica pipeline writes carries the attach triple the original composes for
// the third-person weapon [orig: the held-weapon euler @ entity+0x2B8..0x2C0]:
// a level-standing soldier's weapon comes out at the attach PITCH and the attach
// YAW, roll rides the body roll plus the lean, and the 0x80 hold states flag the
// hand frame. Five poses pin the triple; the US01 rig then pins the anchor the
// shell hangs it on — bone 16 (the weapon hand) rests out along the arm at
// shoulder height in the bind T-pose — and M4_3RD carries user points ahead of
// its origin (muzzle authored +Z).
// Gated on OPENNOVA_JO_ASSETS (an extracted JO tree carrying items.def, US01
// and M4_3RD).
#include "common/retail_paths.h"

#include <base/resource_index/resource_index.h>
#include <base/vfs/vfs.h>
#include <formats/def/def.h>
#include <formats/threedi/threedi_3di3.h>
#include <runtime/inmatch/client_replica_present.h>
#include <runtime/anim/aim_overlay.h>
#include <runtime/simassets/adm_skeletal_clips.h>
#include <runtime/simassets/collision_resolve.h>
#include <runtime/simassets/sim_collision_pose.h>
#include <runtime/simassets/sim_model_cache.h>
#include <runtime/world/angle.h>
#include <runtime/world/infantry.h>
#include <runtime/world/present_rows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace opennova;

int failures = 0;
bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	++failures;
	return false;
}

constexpr int kPlayerRuntimeType = 0x14B9;
constexpr int kHandBone = 16;
constexpr size_t kRecordFloats = 512; // comfortably past the last PF_ index

int32_t bam_deg(double degrees) {
	const double turns = degrees / 360.0;
	return static_cast<int32_t>(static_cast<int64_t>(std::llround(turns * 4294967296.0)));
}

double wrap_deg(double d) {
	while (d >= 180.0) d -= 360.0;
	while (d < -180.0) d += 360.0;
	return d;
}

struct Case {
	const char *name;
	double pitch, yaw, roll;
};

const Case kCases[] = {
	{"level, facing yaw=0", 0.0, 0.0, 0.0},
	{"level, facing yaw=90", 0.0, 90.0, 0.0},
	{"level, facing yaw=-135", 0.0, -135.0, 0.0},
	{"pitch only, +30 (aiming up)", 30.0, 0.0, 0.0},
	{"pitch -20, yaw 45, roll 10", -20.0, 45.0, 10.0},
};

} // namespace

int main() {
	RETAIL_REQUIRE_OR_SKIP(assets, retail::assets(),
			"OPENNOVA_JO_ASSETS (an extracted JO tree carrying items.def, US01 and M4_3RD)");

	// --- The attach triple through the present-record writer, five poses.
	int hand_frame_state = -1;
	for (int s = 50; s <= 61; ++s)
		if ((world::infantry_anim_flags(s) & 0x80u) != 0u) {
			hand_frame_state = s;
			break;
		}
	std::printf("%-30s %8s %8s %8s | %8s %8s %8s\n", "case", "pitch", "yaw", "roll", "rec_p",
			"rec_y", "rec_r");
	for (const Case &c : kCases) {
		anim::AimOverlayInputs in;
		in.aim_pitch = bam_deg(c.pitch);
		in.aim_yaw = bam_deg(90.0 - c.yaw); // mission yaw -> engine heading
		in.roll = bam_deg(c.roll);
		float r[kRecordFloats] = {};
		inmatch::write_present_held_weapon(r, 1, false, in, world::anim_state::kIdle);
		const double rec_p = r[world::PF_HELD_WEAPON_PITCH_DEG];
		const double rec_y = wrap_deg(r[world::PF_HELD_WEAPON_YAW_DEG]);
		const double rec_r = r[world::PF_HELD_WEAPON_ROLL_DEG];
		std::printf("%-30s %8.2f %8.2f %8.2f | %8.2f %8.2f %8.2f\n", c.name, c.pitch, c.yaw, c.roll,
				rec_p, rec_y, rec_r);
		char msg[160];
		std::snprintf(msg, sizeof(msg), "%s: the record carries the equipped adm", c.name);
		expect(r[world::PF_HELD_WEAPON_ADM] == 1.0f, msg);
		std::snprintf(msg, sizeof(msg), "%s: elevation tracks the attach PITCH", c.name);
		expect(std::fabs(rec_p - c.pitch) < 0.01, msg);
		std::snprintf(msg, sizeof(msg), "%s: bearing tracks the attach YAW", c.name);
		expect(std::fabs(wrap_deg(rec_y - c.yaw)) < 0.01, msg);
		std::snprintf(msg, sizeof(msg), "%s: roll rides the body roll", c.name);
		expect(std::fabs(rec_r - c.roll) < 0.01, msg);
		std::snprintf(msg, sizeof(msg), "%s: an ordinary hold uses the entity frame", c.name);
		expect(r[world::PF_HELD_WEAPON_HAND_FRAME] == 0.0f, msg);
	}
	{
		anim::AimOverlayInputs in;
		in.aim_pitch = bam_deg(30.0);
		in.lean = bam_deg(5.0);
		in.roll = bam_deg(4.0);
		float r[kRecordFloats] = {};
		if (hand_frame_state >= 0) {
			inmatch::write_present_held_weapon(r, 1, false, in, hand_frame_state);
			expect(r[world::PF_HELD_WEAPON_HAND_FRAME] == 1.0f,
					"a 0x80 hold state flags the hand frame");
		}
		expect(std::fabs(r[world::PF_HELD_WEAPON_ROLL_DEG] - 9.0) < 0.01,
				"the lean adds to the body roll");
		std::memset(r, 0, sizeof(r));
		inmatch::write_present_held_weapon(r, 1, true, in, world::anim_state::kIdle);
		expect(r[world::PF_HELD_WEAPON_ADM] == 0.0f, "a dead body writes no held weapon");
		in.rolling = true;
		std::memset(r, 0, sizeof(r));
		inmatch::write_present_held_weapon(r, 1, false, in, world::anim_state::kIdle);
		expect(std::fabs(r[world::PF_HELD_WEAPON_ROLL_DEG] - 5.0) < 0.01,
				"a rolling body drops the body roll and keeps the lean");
	}

	// --- The anchor: US01 bone 16 rests at identity; M4_3RD points +Z.
	ResourceIndex index;
	if (!index.scan(assets) && !index.scan(assets, std::string(), VfsMountMode::LooseOnly))
		return retail::skip("a mountable OPENNOVA_JO_ASSETS tree");
	std::vector<uint8_t> items_bytes;
	if (!index.read_file("items.def", items_bytes)) return retail::skip("items.def under OPENNOVA_JO_ASSETS");
	DefItemsFile items{};
	if (def_parse_items_memory(items_bytes.data(), items_bytes.size(), &items) != 0)
		return retail::skip("a parseable items.def");
	const DefItemDef *def = simassets::find_item_def(
			items, simassets::visual_item_id_for_runtime_type(kPlayerRuntimeType, items));
	simassets::SimModelCache models;
	models.set_index(&index);
	const Threedi3di3 *body = def != nullptr ? models.model_for(def->graphic) : nullptr;
	if (expect(body != nullptr, "the player's visual model loads")) {
		std::vector<anim::Vec3> origins;
		std::vector<int> parents;
		simassets::model_bone_table(*body, origins, parents);
		std::string adm(def->anim_def);
		if (adm.size() < 4 || adm.compare(adm.size() - 4, 4, ".adm") != 0) adm += ".adm";
		simassets::AdmSkeletalClips clips;
		if (expect(clips.load_from_adm(&index, adm, origins, parents), "the player's rig loads") &&
				expect(clips.bone_count() > static_cast<size_t>(kHandBone) && clips.fk_valid(),
						"the rig carries bone 16 with a valid FK chain")) {
			const simassets::AdmSkeletalClips::RestTransform &rest = clips.rest_global()[kHandBone];
			std::printf("held_weapon_attach: %s bone %d rest origin (%.3f, %.3f, %.3f)\n",
					clips.adm_name().c_str(), kHandBone, rest.origin.x, rest.origin.y, rest.origin.z);
			expect(std::fabs(rest.origin.x) > 0.3f, "bone 16 rests out along the arm (bind T-pose)");
			expect(rest.origin.y > 0.0f, "bone 16 rests above the pelvis origin");
		}
	}
	def_free_items(&items);
	if (const Threedi3di3 *m4 = models.model_for("M4_3RD")) {
		int ahead = 0;
		for (size_t i = 0; i < m4->user_point_count; ++i) {
			float p[3];
			threedi_user_point_position(&m4->user_points[i], p);
			if (p[2] > 0.1f) ++ahead;
		}
		std::printf("held_weapon_attach: M4_3RD user points %zu (%d ahead of the origin)\n",
				m4->user_point_count, ahead);
		expect(ahead > 0, "M4_3RD has user points ahead of its origin (muzzle authored +Z)");
	} else {
		expect(false, "M4_3RD loads");
	}

	if (failures == 0) std::printf("held_weapon_attach: OK\n");
	return failures == 0 ? 0 : 1;
}
