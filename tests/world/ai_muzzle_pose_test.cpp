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
#include <runtime/anim/skeletal_clips.h>

#include <cmath>
#include <cstdio>
#include <string>

using namespace opennova::threedi;

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
	if (!expect(rig.local.has_local_player(), "the host's own player spawned")) return 1;
	rig.install_weapon("WPN_M4AUTO");
	for (int t = 0; t < 62; ++t) rig.tick(); // one settled second of posing
	expect(rig.world.pose_provider != nullptr, "the world carries a muzzle pose provider");
	if (rig.world.pose_provider == nullptr) return 1;

	// --- Every live foot NPC resolves a muzzle inside the rifle envelope.
	int persons = 0, stamped = 0, good = 0, misses = 0, head_height = 0, guards = 0;
	for (int i = 0; i < rig.world.ai.count(); ++i) {
		const w::AiEntity *e = rig.world.ai.at(i);
		if (e == nullptr || !e->inf.active) continue;
		const w::Entity *ent = rig.world.registry.get(e->handle);
		if (ent == nullptr || !ent->alive || ent->mounted || ent->item_type != kItemTypePerson) continue;
		if (ent == rig.local.player()) continue;
		++persons;
		int32_t out[3] = {};
		if (!rig.world.pose_provider->resolve_muzzle_pose(rig.world, e->handle, out)) continue;
		++stamped;
        int32_t indexed[3] = {};
        expect(e->profile.organic.launch[0] != 0, "the DEF closeattack point is bound");
        const bool indexed_ok = rig.world.pose_provider->resolve_organic_attachment(
                rig.world, e->handle, e->profile.organic.launch[0], indexed);
        expect(indexed_ok, "the bound organic launch byte resolves on the live skeleton");
        if (indexed_ok) {
            expect(indexed[0] == out[0] && indexed[1] == out[1] && indexed[2] == out[2],
                    "indexed and named closeattack queries produce the same posed point");
        }
        for (int slot = 1; slot < 3; ++slot) {
            if (e->profile.organic.launch[slot] == 0) continue;
            expect(rig.world.pose_provider->resolve_organic_attachment(
                    rig.world, e->handle, e->profile.organic.launch[slot], indexed),
                    "each authored rocket/marker launch resolves independently");
        }
        expect(!rig.world.pose_provider->resolve_organic_attachment(
                rig.world, e->handle, 0, indexed), "point zero takes the caller's fallback");
        if (stamped == 1 && indexed_ok) {
            w::AiEntity *live = rig.world.ai.for_handle(e->handle);
            const int32_t saved_x = live->pos[0];
            // One whole unit survives the skeletal float-matrix conversion
            // exactly at CP01's coordinates; sub-unit Q16 deltas round there.
            live->pos[0] += 65536; // motor position leads the registry/presentation mirror
            const bool moved = rig.world.pose_provider->resolve_organic_attachment(
                    rig.world, e->handle, e->profile.organic.launch[0], indexed);
            expect(moved && indexed[0] == out[0] + 65536 &&
                    indexed[1] == out[1] && indexed[2] == out[2],
                    "attachment queries use the live fixed-point motor position");
            live->pos[0] = saved_x;
        }
        int32_t head[3], hand[3];
        const bool anchors = rig.world.pose_provider->resolve_skeletal_anchor(
                rig.world, e->handle, w::SkeletalAnchor::Head, head) &&
                rig.world.pose_provider->resolve_skeletal_anchor(
                rig.world, e->handle, w::SkeletalAnchor::HeldWeapon, hand);
        expect(anchors, "the NPC's live head and weapon-hand anchors resolve");
        if (anchors) {
            const double separation = std::sqrt(
                    std::pow(double(head[0]) - hand[0], 2) +
                    std::pow(double(head[1]) - hand[1], 2) +
                    std::pow(double(head[2]) - hand[2], 2)) / 65536.0;
            expect(separation > 0.01 && separation < 4.0,
                    "posed head and hand remain distinct and inside one person's reach");
        }
		const w::Vec3 origin = testrig::ai_position(*e);
		const float up = fx(out[2]) - origin.z;
		const float horiz = std::hypot(fx(out[0]) - origin.x, fx(out[1]) - origin.y);
		const bool in_envelope = up > 0.0f && up < 0.8f && horiz > 0.1f && horiz < 2.0f;
		// A guard (Flags 0x40; CP01 authors 49 Guarding organics) holds its post in
		// the guard family with the rifle lowered, below the standing envelope.
		// [orig: Entity_UpdateInfantryAI @0x4BD196..0x4BD231]
		const bool guard_pose = e->inf.anim_state >= 140 && e->inf.anim_state <= 144;
		if (stamped <= 8 || (!in_envelope && misses < 8))
			std::printf("muzzle: net=%d team=%d up=%.2f horiz=%.2f pos=(%.1f, %.1f, %.1f) anim=%d%s\n",
					int(ent->net_id), int(e->team), up, horiz, origin.x, origin.y, origin.z,
					e->inf.anim_state, in_envelope ? "" : " (outside the rifle envelope)");
		if (guard_pose) ++guards;
		else if (in_envelope) ++good;
		else ++misses;
		if (up > 0.82f && horiz < 0.05f) ++head_height;
	}
	std::printf("muzzle: %d foot NPCs, %d resolved, %d guards, %d in the rifle envelope, "
			"%d at head height\n", persons, stamped, guards, good, head_height);
	expect(persons > 0, "CP01 carries foot NPCs");
	expect(stamped > 0, "at least one NPC resolved a muzzle (the provider is registered)");
	expect(head_height == 0, "no muzzle sits at the head-height fallback point");
	// Crouched/prone bodies and the odd mid-transition pose sit outside the
	// standing rifle envelope; the rest of CP01 lands ~9 in 10 inside it.
	expect(good * 5 >= (stamped - guards) * 4,
			"at least four in five stamped non-guard muzzles land in the rifle envelope");

	// --- The player's own rig: US01's head and hand pivots, and the held model.
	const w::Entity *pe = rig.local.player();
	int32_t pm[3] = {};
	const bool player_muzzle = rig.world.pose_provider->resolve_muzzle_pose(
			rig.world, pe->handle, pm);
	std::printf("muzzle: player launch point resolved=%d (the local player's fire pass reads the viewmodel)\n",
			int(player_muzzle));
	const anim::SkeletalClips *rig_clips = rig.collision_pose.skeletal_rig(pe->handle);
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
	if (const Threedi3di3 *m4 = rig.assets().model("M4_3RD").get()) {
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
