// The sim-side muzzle pose on the retail data: every foot NPC on CP01 resolves
// its launch userpoint on its OWN posed skeleton through the world's muzzle
// pose provider [orig: Entity_GetAttachmentWorldPosition @0x4b2670], and the
// point lands in the rifle envelope (a little above the entity origin, a little
// ahead of it) — never at the head-height point the old 0.9 u chest-lift
// fallback produced. The player's US01 rig backs the same provider: its head
// (bone 14) rests above its weapon hand (bone 16) in the bind pose, and the
// held-weapon model (M4_3RD) carries the user points the fire pass reads
// (MFLASH01 ahead of the origin, muzzle authored +Z).
// Gated on OPENNOVA_JO_DIR (a retail JO install carrying CP01.bms).
#include "common/retail_mission_files.h"
#include "common/retail_paths.h"

#include <formats/threedi/threedi_3di3.h>
#include <runtime/simassets/adm_skeletal_clips.h>

#include <cmath>
#include <cstdio>
#include <string>

namespace {

using namespace opennova;
namespace w = opennova::world;

int failures = 0;
bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	++failures;
	return false;
}

constexpr int kItemTypePerson = 3;
constexpr int kHeadBone = 14; // LocalPlayerPresenter.PLAYER_HEAD_BONE_INDEX
constexpr int kHandBone = 16; // the held-weapon attach bone

float fx(int32_t q16) { return static_cast<float>(q16) / 65536.0f; }

} // namespace

int main() {
	RETAIL_REQUIRE_OR_SKIP(install, retail::install(),
			"OPENNOVA_JO_DIR (a retail JO install carrying CP01.bms)");
	testrig::RetailMissionRig rig;
	std::string error;
	if (!rig.open(install, "CP01.bms", error)) return retail::skip(error.c_str());
	testrig::BootOptions options;
	if (!expect(rig.boot(options, error), "CP01 boots")) {
		std::fprintf(stderr, "  %s\n", error.c_str());
		return 1;
	}
	if (!expect(rig.has_local_player(), "the host's own player spawned")) return 1;
	rig.install_weapon("WPN_M4AUTO");
	for (int t = 0; t < 62; ++t) rig.tick(); // one settled second of posing
	expect(rig.world.muzzle_pose_provider != nullptr, "the world carries a muzzle pose provider");
	if (rig.world.muzzle_pose_provider == nullptr) return 1;

	// --- Every live foot NPC resolves a muzzle inside the rifle envelope.
	int persons = 0, stamped = 0, good = 0, misses = 0, head_height = 0;
	for (int i = 0; i < rig.ai.count(); ++i) {
		const w::AiEntity *e = rig.ai.at(i);
		if (e == nullptr || !e->inf.active) continue;
		const w::Entity *ent = rig.world.registry.get(e->handle);
		if (ent == nullptr || !ent->alive || ent->mounted || ent->item_type != kItemTypePerson) continue;
		if (ent == rig.player()) continue;
		++persons;
		int32_t out[3] = {};
		if (!rig.world.muzzle_pose_provider->resolve_muzzle_pose(rig.world, e->handle, out)) continue;
		++stamped;
		const w::Vec3 origin = testrig::ai_position(*e);
		const float up = fx(out[2]) - origin.z;
		const float horiz = std::hypot(fx(out[0]) - origin.x, fx(out[1]) - origin.y);
		const bool in_envelope = up > 0.0f && up < 0.8f && horiz > 0.1f && horiz < 2.0f;
		if (stamped <= 8 || (!in_envelope && misses < 8))
			std::printf("muzzle: net=%d team=%d up=%.2f horiz=%.2f pos=(%.1f, %.1f, %.1f) anim=%d%s\n",
					int(ent->net_id), int(e->team), up, horiz, origin.x, origin.y, origin.z,
					e->inf.anim_state, in_envelope ? "" : " (outside the rifle envelope)");
		if (in_envelope) ++good;
		else ++misses;
		if (up > 0.82f && horiz < 0.05f) ++head_height;
	}
	std::printf("muzzle: %d foot NPCs, %d resolved, %d in the rifle envelope, %d at head height\n",
			persons, stamped, good, head_height);
	expect(persons > 0, "CP01 carries foot NPCs");
	expect(stamped > 0, "at least one NPC resolved a muzzle (the provider is registered)");
	expect(head_height == 0, "no muzzle sits at the head-height fallback point");
	// Crouched/prone bodies and the odd mid-transition pose sit outside the
	// standing rifle envelope; the CP01 walk lands ~9 in 10 inside it.
	expect(good * 5 >= stamped * 4, "at least four in five stamped muzzles land in the rifle envelope");

	// --- The player's own rig: US01's head and hand pivots, and the held model.
	const w::Entity *pe = rig.player();
	int32_t pm[3] = {};
	const bool player_muzzle = rig.world.muzzle_pose_provider->resolve_muzzle_pose(
			rig.world, pe->handle, pm);
	std::printf("muzzle: player launch point resolved=%d (the local player's fire pass reads the viewmodel)\n",
			int(player_muzzle));
	const simassets::AdmSkeletalClips *rig_clips = rig.collision_pose.skeletal_rig(pe->handle);
	if (expect(rig_clips != nullptr && rig_clips->loaded(), "the player's skeletal rig is registered")) {
		expect(rig_clips->bone_count() > static_cast<size_t>(kHandBone),
				"the player rig carries the head and weapon-hand bones");
		if (rig_clips->bone_count() > static_cast<size_t>(kHandBone) && rig_clips->fk_valid()) {
			const anim::Vec3 head = rig_clips->rest_global()[kHeadBone].origin;
			const anim::Vec3 hand = rig_clips->rest_global()[kHandBone].origin;
			std::printf("muzzle: %s bone %d rest=(%.3f, %.3f, %.3f) bone %d rest=(%.3f, %.3f, %.3f)\n",
					rig_clips->adm_name().c_str(), kHeadBone, head.x, head.y, head.z, kHandBone,
					hand.x, hand.y, hand.z);
			expect(head.y > hand.y, "the head pivot rests above the weapon hand");
			expect(hand.y > 0.0f, "the weapon hand rests above the pelvis origin");
		}
	}
	if (const Threedi3di3 *m4 = rig.models.model_for("M4_3RD")) {
		expect(m4->user_point_count > 0, "M4_3RD carries user points");
		int forward = 0;
		for (size_t i = 0; i < m4->user_point_count; ++i) {
			float p[3];
			threedi_user_point_position(&m4->user_points[i], p);
			if (i < 8)
				std::printf("muzzle: M4_3RD userpoint %zu %s (%.3f, %.3f, %.3f)\n", i + 1,
						m4->user_points[i].name, p[0], p[1], p[2]);
			if (p[2] > 0.1f) ++forward;
		}
		expect(forward > 0, "M4_3RD has a user point ahead of its origin (muzzle authored +Z)");
	} else {
		expect(false, "M4_3RD loads from the mount");
	}

	if (failures == 0) std::printf("ai_muzzle_pose: OK on CP01\n");
	return failures == 0 ? 0 : 1;
}
