// The tick digest (ADR 0043): one hash over the simulation's per-tick state,
// chained across N ticks, so a STRUCTURAL slice (a free function becoming a
// method, a field regrouped, a tick leg moved between owners) proves it changed
// nothing observable: the digest before equals the digest after.
//
// What is hashed, per tick, in a fixed order: the logic clock and both RNG
// owners; every registry row's identity, pose, health and flag words; every
// AI component's 16.16 pose, velocity, health and the 203 brain dwords; the
// script variable store; the match clock, roster size and frozen result. Any
// reorder of a tick leg that consumes an RNG draw, moves a body, or changes an
// AI decision changes the chain.
//
// Two legs:
//   default          the SYNTHETIC mission (ungated, every CI run): boots the
//                    same in-memory mission twice and asserts (a) both boots
//                    produce the same chain (determinism) and (b) the chain
//                    equals the committed kSyntheticDigest. A slice whose
//                    change is genuinely behavior-neutral leaves the constant
//                    alone; `--print` reports the current value for the rare
//                    ledgered follow-up that is allowed to move it.
//   --mission NAME   a retail mission through the ctest rig (local tool, never
//                    a ctest leg: needs the OPENNOVA_JO_DIR root), printing
//                    `digest <mission> <ticks> <hex>` for scripts/parity/
//                    tick_digest.py --record / --check. Float codegen differs
//                    across compilers, so a retail digest is compared on one
//                    machine and never committed.
#include "common/boot_file_source.h"
#include "common/retail_mission_files.h"
#include "common/retail_paths.h"

#include <runtime/inmatch/local_role.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/world/ai.h>
#include <runtime/world/match.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>


// The bare no-net tick: the local role over the kernel (ADR 0043 d3; the
// kernel itself owns no tick).
static void tick_no_net(opennova::mission::MissionKernel &kernel) {
	opennova::inmatch::LocalRole role;
	role.bind(kernel);
	role.run_tick(opennova::inmatch::TickInput{});
}
using namespace opennova;
namespace ms = opennova::mission;
namespace w = opennova::world;

namespace {

// The committed chain for the synthetic mission below over kSyntheticTicks
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
constexpr uint64_t kSyntheticDigest = 0xa22209beb52b4e4fULL;
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
	template <class T>
	void value(const T &v) {
		bytes(&v, sizeof(v));
	}
};

void hash_entity(Digest &d, const w::Entity &e) {
	d.value(e.net_id);
	d.value(e.bms_id);
	d.value(static_cast<int32_t>(e.kind));
	d.value(e.item_id);
	d.value(e.position.x);
	d.value(e.position.y);
	d.value(e.position.z);
	d.value(e.yaw);
	d.value(e.pitch);
	d.value(e.roll);
	d.value(e.health);
	d.value(e.alive);
	d.value(e.team);
	d.value(e.group_id);
	d.value(e.flags);
	d.value(e.engine_flags);
	d.value(e.ai_flags);
}

void hash_ai(Digest &d, const w::AiEntity &a) {
	d.value(a.pos[0]);
	d.value(a.pos[1]);
	d.value(a.pos[2]);
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
void hash_world(Digest &d, const w::World &world) {
	d.value(world.logic_tick);
	d.value(world.prng16_state);
	d.value(world.crt_rand.state);
	world.registry.for_each([&](const w::Entity &e) {
		hash_entity(d, e);
		if (const w::AiEntity *a = world.ai.for_handle(e.handle)) hash_ai(d, *a);
	});
	for (int i = 0; i < w::ScriptVarStore::kMissionVars; ++i) d.value(world.script.vars.get_mission(i));
	for (int i = 0; i < w::ScriptVarStore::kGlobalVars; ++i) d.value(world.script.vars.get_global(i));
	d.value(world.match.remaining_ticks());
	d.value(static_cast<uint32_t>(world.match.players().size()));
	const w::MatchResult &result = world.match.result();
	d.value(result.ready);
	d.value(result.winner_team);
	d.value(result.team_scores[0]);
	d.value(result.team_scores[1]);
}

// --- the synthetic mission ---------------------------------------------------

bms::Entity organic(int32_t x, int32_t y, int32_t z, uint8_t team, int32_t yaw) {
	bms::Entity e{};
	e.type = bms::ItemType::Organic;
	e.x = x;
	e.y = y;
	e.z = z;
	e.yaw = yaw;
	e.team = team;
	return e;
}

bms::Entity item(int32_t type_id, int32_t x, int32_t y, int32_t z) {
	bms::Entity e{};
	e.type = bms::ItemType::Item;
	e.type_id = type_id;
	e.x = x;
	e.y = y;
	e.z = z;
	return e;
}

using test_boot::source_over;

// Two opposing squads plus a few items, and a WAC layer whose writes are part
// of the chain. The kernel's own player is the one human the bare tick
// counts, so neither the WAC tick nor the entity update holds.
bms::File synthetic_mission() {
	bms::File m{};
	int32_t next_id = 20;
	for (int i = 0; i < 6; ++i) {
		m.organics.push_back(organic((10 + i * 4) << 16, 10 << 16, 0, /*team=*/1, 90));
		m.organics.back().id = next_id++;
	}
	for (int i = 0; i < 6; ++i) {
		m.organics.push_back(organic((10 + i * 4) << 16, 60 << 16, 0, /*team=*/2, 270));
		m.organics.back().id = next_id++;
	}
	m.items.push_back(item(/*type_id=*/164, 30 << 16, 35 << 16, 3 << 16));
	m.items.back().id = next_id++;
	m.items.push_back(item(/*type_id=*/164, 40 << 16, 35 << 16, 3 << 16));
	m.items.back().id = next_id++;
	m.events.push_back(bms::Event{});
	return m;
}

bool synthetic_chain(int ticks, uint64_t &out, std::string &error) {
	std::map<std::string, std::string> files;
	files["synth.wac"] = "if elapse(1) then set(v1,1) endif\nif elapse(2) then inc(v1) endif\n";
	ms::MissionKernel kernel;
	kernel.open_document(synthetic_mission(), "synth", source_over(&files));
	ms::KernelBootOptions options;
	if (!kernel.boot(options, error)) return false;
	Digest d;
	hash_world(d, kernel.world);
	for (int i = 0; i < ticks; ++i) {
		tick_no_net(kernel);
		hash_world(d, kernel.world);
	}
	out = d.h;
	return true;
}

// --- the retail leg (local tool) ---------------------------------------------

int retail_chain(const std::string &mission_name, int ticks, bool listen) {
	const std::string root = retail::install();
	if (root.empty()) {
		std::printf("SKIP: needs OPENNOVA_JO_DIR for --mission\n");
		return retail::kSkipExitCode;
	}
	testrig::RetailMissionRig rig;
	std::string error;
	if (!rig.open(root, mission_name, error)) {
		std::printf("FAIL: open %s: %s\n", mission_name.c_str(), error.c_str());
		return 1;
	}
	testrig::BootOptions options;
	options.listen_server = listen;
	// The listen host mints its host key, start tick and session seed from the
	// clock (the seed also seeds the world's CRT rand stream), so pin all three
	// or two runs of the same mission never agree.
	rig.host.host_key = 0x00C0FFEEu;
	rig.host.host_start_tick = 1000u;
	rig.host.session_seed_id = 123456u;
	if (!rig.boot(options, error)) {
		std::printf("FAIL: boot %s: %s\n", mission_name.c_str(), error.c_str());
		return 1;
	}
	Digest d;
	hash_world(d, rig.world);
	for (int i = 0; i < ticks; ++i) {
		rig.tick();
		hash_world(d, rig.world);
	}
	std::printf("digest %s %d %016llx\n", mission_name.c_str(), ticks,
			static_cast<unsigned long long>(d.h));
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	std::string mission;
	int ticks = kSyntheticTicks;
	bool listen = true;
	bool print = false;
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "--mission") == 0 && i + 1 < argc) mission = argv[++i];
		else if (std::strcmp(argv[i], "--ticks") == 0 && i + 1 < argc) ticks = std::atoi(argv[++i]);
		else if (std::strcmp(argv[i], "--no-listen") == 0) listen = false;
		else if (std::strcmp(argv[i], "--print") == 0) print = true;
	}
	if (!mission.empty()) return retail_chain(mission, ticks, listen);

	std::string error;
	uint64_t first = 0, second = 0;
	if (!synthetic_chain(ticks, first, error)) {
		std::printf("FAIL: synthetic boot: %s\n", error.c_str());
		return 1;
	}
	if (!synthetic_chain(ticks, second, error)) {
		std::printf("FAIL: synthetic boot (second): %s\n", error.c_str());
		return 1;
	}
	std::printf("digest synthetic %d %016llx\n", ticks, static_cast<unsigned long long>(first));
	if (first != second) {
		std::printf("FAIL: two identical boots diverged (%016llx vs %016llx): the tick is not "
					"deterministic\n",
				static_cast<unsigned long long>(first), static_cast<unsigned long long>(second));
		return 1;
	}
	if (print) return 0;
	if (ticks == kSyntheticTicks && first != kSyntheticDigest) {
		std::printf("FAIL: synthetic digest %016llx != committed %016llx. A structural slice must "
					"not move it; a ledgered behavior change re-pins kSyntheticDigest and says why.\n",
				static_cast<unsigned long long>(first),
				static_cast<unsigned long long>(kSyntheticDigest));
		return 1;
	}
	std::printf("tick_digest: OK\n");
	return 0;
}
