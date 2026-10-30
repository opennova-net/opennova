// The tick digest's chain (ADR 0043): one hash over the simulation's per-tick state, chained
// across N ticks, and the synthetic mission's committed value with what it boots over. The
// tick_digest ctest (mission/tick_digest_test.cpp) runs the chain on the bare no-net rig;
// tests/null_backend runs the same mission through the platform seams and pins the same value, so a
// seam that changed a tick shows. Header-only, test infrastructure (no retail counterpart of its
// own: the cites below are the digest moves' witnesses).
//
// What is hashed, per tick, in a fixed order: the logic clock and both RNG owners; every registry
// row's identity, pose, health and flag words; every AI component's 16.16 pose, velocity, health
// and the 203 brain dwords; the script variable store; the match clock, roster size and frozen
// result.
#pragma once

#include "common/synthetic_mission.h"

#include <formats/mission/bms.h>
#include <runtime/world/ai.h>
#include <runtime/world/match.h>
#include <runtime/world/world.h>

#include <cstddef>
#include <cstdint>

namespace test_digest {

namespace w = opennova::world;

// The committed chain for the synthetic mission (tick_digest_test.cpp) over kSyntheticTicks
// ticks. Moves ONLY with a ledgered behavior change; a structural slice that
// moves it has changed behavior and must say why.
// WAC retail timing (world-wac-ai-re.md section 33): both elapse blocks fire
// at boot, then the no-human gate pauses the VM. V1 stays 2. Replaying only
// the previous V1 timeline (0 until tick 63, 1 until 125, then 2) recovers
// 0x116f65ec9ea08922 exactly; entity, AI and RNG state did not move.
// D-WAC-10 (world-wac-ai-re.md section 33.15a) removes the detached 16-word
// music bank from World: M# now addresses the actual compiled MUS context.
// The synthetic mission has no music context. Restoring ONLY those 16 zero
// DWORDs to each of the 241 hash samples recovers e33cefc459163b68 exactly;
// entity, AI, RNG and real script state are unchanged by this digest update.
// The AI/script parity pass ports retail's truncating spawn angle,
// ((deg << 16) / 360) << 16 [orig: Entity_SpawnFromBMSRecord @0x40EB42..0x40EB66],
// in place of deg * 11930464: every spawned heading loses its low 16 bits,
// which moves the chain from 0x18f8080dd8fcdb68.
// The same pass keys the brain class init on the item class, not the profile
// type [orig: Entity_InitHelicopterAIFromDef @0x4683C0 / Entity_InitVehicleAIFromDef
// @0x4686C0], and drops the invented default speed of 20: organic brains no
// longer carry it in brain[49]/[50], which moves the chain from 0x31938283c4bbc368
// (bisected to that one commit; the brain stream reproduced the old value by
// restoring only the default speed).
// The pass then ports the entity-update admission: an authority with no human
// in the world and a WAC clock past its first run skips the whole entity update
// [orig: Game_ProcessMainFrame @0x526703..0x526742]. The synthetic mission runs
// WAC with no human, so its world holds after the first tick, which moves the
// chain from 0xe8c5a2c197da3de8 (the commit before it still gives that value).
// A held world covers almost nothing, so the chain now counts one human, as a
// played mission has: the WAC tick and the entity update run every tick
// again, which moves the chain from 0xe5830c6fd01c9439.
// The org1 think head then seeds the aim pitch from the SSN on every think and
// skips an airborne body's think [orig: Entity_UpdateInfantryAI
// @0x4BAA4B..0x4BAA66], which moves the chain from 0xc35a881f8f092209
// (reverting that one commit restores it).
// The org1 update then runs nothing for a body with no animation channel
// [orig: Entity_UpdateInfantryAI @0x4B9931..0x4B993B], and an item naming no
// table registers none. The synthetic people named no item, so they would
// stand and the chain would cover no think (0x00436028504af8e0); the chain
// instead gives them an items.def row naming the minted soldier.adm (over
// idle.bad and walk.bad), and the player's row the same. Their channels now
// play those clips, which moves the chain from 0xa22209beb52b4e4f; with every
// person naming its table the exit itself moves nothing (reverting it gives
// the same value).
// The registry row now carries the Yaw word (D-NET-922) and the chain
// digests it in place of the whole-degree mirror, which moves it from
// 0x517e7adde3a32d91; digesting the word's rounded whole degrees gives that
// value back, so no tick of this synthetic run moved.
// The synthetic mission has no player-start marker, so its player keeps the
// zeroed entity's heading word 0, as retail's mission-start spawn leaves it
// (D-NET-817), where the spawn had stored the placed default
// 0x40000000; that moves the chain from 0xe722d011570202ed (restoring the
// default gives that value back).
// The registry row carries its Pitch / Roll words (D-NET-923) and the
// chain digests them in place of the whole-degree mirrors, which moves it from
// 0xdcc41c0472d7226d; digesting the words' rounded whole degrees gives that
// value back, so no tick of this synthetic run moved (it carries no section
// piece and no body on a tilted placed row).
// The player's body binds the row its type word names (D-NET-348: 0x14B9 is
// "Player #1, Multiplayer", 105305; the presentation's 105310 stand-in is gone),
// so the player's row is that one: its traits sweep now finds a def where it
// found none and its table the same soldier rows. Together with the mission
// start running at the zeroed frame tick (D-NET-515, f69c11f61: the start's end
// stores the first frame's 1), this moves the chain from 0xa7c762a945b847fd to
// 0xc5d0b62b98fd422f, the value CI printed on push 137 (run 39686345393). It is
// this deterministic stream's own value (a self-consistency pin, not a retail
// value).
// A body's update is its bound row's move function (D-NET-892: a row naming
// none binds the null row, which no update runs), so the people's row names org1 and
// the player's org2: the updates they ran by the Player bit before, and the chain
// does not move.
constexpr uint64_t kSyntheticDigest = 0xc5d0b62b98fd422fULL;
constexpr int kSyntheticTicks = 240;

struct Digest {
	uint64_t h = 1469598103934665603ULL; // FNV-1a 64 offset basis

	void bytes(const void *data, size_t n) {
		const auto *p = static_cast<const unsigned char *>(data);
		for (size_t i = 0; i < n; ++i) {
			h ^= p[i];
			h *= 1099511628211ULL;
		}
	}
	template <class T> void value(const T &v) { bytes(&v, sizeof(v)); }
};

inline void hash_entity(Digest &d, const w::Entity &e) {
	d.value(e.net_id);
	d.value(e.bms_id);
	d.value(static_cast<int32_t>(e.kind));
	d.value(e.item_id);
	// The float view of the 16.16 words (D-NET-507): the float the chain always hashed.
	d.value(static_cast<float>(e.position.x));
	d.value(static_cast<float>(e.position.y));
	d.value(static_cast<float>(e.position.z));
	d.value(e.heading);
	d.value(e.pitch_bam);
	d.value(e.roll_bam);
	d.value(e.health);
	d.value(e.alive);
	d.value(e.team);
	d.value(e.group_id);
	d.value(e.flags);
	d.value(e.engine_flags);
	d.value(e.ai_flags);
}

inline void hash_ai(Digest &d, const w::AiEntity &a) {
	d.value(a.self->position.x.q16);
	d.value(a.self->position.y.q16);
	d.value(a.self->position.z.q16);
	d.value(a.heading);
	d.value(a.pitch);
	d.value(a.roll);
	d.value(a.body_pitch);
	d.value(a.vel_x);
	d.value(a.vel_y);
	d.value(a.health);
	d.value(a.team);
	d.bytes(a.brain.f, sizeof(a.brain.f));
}

// One tick's worth of state folded into the running chain.
inline void hash_world(Digest &d, const w::World &world) {
	d.value(world.logic_tick);
	d.value(world.prng16_state);
	d.value(world.crt_rand.state);
	world.registry.for_each([&](const w::Entity &e) {
		hash_entity(d, e);
		if (const w::AiEntity *a = world.ai.for_handle(e.handle))
			hash_ai(d, *a);
	});
	for (int i = 0; i < w::ScriptVarStore::kMissionVars; ++i)
		d.value(world.script.vars.get_mission(i));
	for (int i = 0; i < w::ScriptVarStore::kGlobalVars; ++i)
		d.value(world.script.vars.get_global(i));
	d.value(world.match.remaining_ticks());
	d.value(static_cast<uint32_t>(world.match.players().size()));
	const w::MatchResult &result = world.match.result();
	d.value(result.ready);
	d.value(result.winner_team);
	d.value(result.team_scores[0]);
	d.value(result.team_scores[1]);
}

// The kernel's assets for the synthetic mission: the minted soldier.adm (its
// idle and walk rows) over the minted idle.bad and walk.bad, read from fixtures/.
// The people's item names it (`anim_def soldier`), as the player's does (the
// rows CRLF-terminated, as a shipped items.def's are).
constexpr int kPeopleTypeId = 2001; // items.def 102001
constexpr const char *kPeopleItems =
		"begin \"Null\"\r\nid 100000\r\ntype marker\r\nend\r\n"
		"begin \"Synthetic soldier\"\r\nid 102001\r\ntype person\r\nanim_def soldier\r\n"
		"move_function org1\r\nend\r\n"
		"begin \"Player #1, Multiplayer\"\r\n"
		"id 105305\r\ntype person\r\nanim_def soldier\r\nmove_function org2\r\nend\r\n";

// The synthetic mission's WAC layer: both elapse blocks fire at boot (world-wac-ai-re.md section
// 33).
constexpr const char *kSyntheticWac =
		"if elapse(1) then set(v1,1) endif\nif elapse(2) then inc(v1) endif\n";
// The people's table's files, under fixtures/anim/.
constexpr const char *kPeopleClipFiles[] = { "soldier.adm", "idle.bad", "walk.bad" };

// The synthetic mission with every person on the people's row.
inline opennova::bms::File people_mission() {
	opennova::bms::File mission = test_mission::synthetic_mission();
	for (opennova::bms::Entity &person : mission.organics)
		person.type_id = kPeopleTypeId;
	return mission;
}

} // namespace test_digest
