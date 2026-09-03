// The joiner role's frame (ADR 0043 d3, slice E8b; ex the S10a joiner world
// bridge). The frame's phase SEQUENCE is the witnessed retail client frame
// [orig: Game_ProcessMainFrame @0x5263f0; Client_ProcessNetworkFrame
// @0x42c180] — this pins the portable contract through its effects: the
// ClientHello one-shot over the socket seam, the per-frame clock, the in-match
// spawn edge for L (H latched, the join-wait latches cleared, the weapon pump
// gated open the same frame), the mid-frame loadout-grant stamp, and the
// per-session latch reset. World-effect depth (health folds, mount sync,
// mirrors) is covered by the GUT net suites and the live LAN pair.

#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/joiner_role.h>
#include <runtime/mission/mission_kernel.h>

#include <runtime/world/ai.h>
#include <runtime/world/entity.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/player_loadout.h>
#include <runtime/world/player_weapon.h>
#include <runtime/world/weapon_inventory.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace opennova;
namespace inmatch = opennova::inmatch;
namespace w = opennova::world;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

const std::string kClientScrk = "CLIENT-BRIDGE-SCRK";
const std::string kServerScrk = "SERVER-BRIDGE-SCRK";
constexpr uint32_t kClientKey = 0x0000BEEFu;
constexpr uint32_t kSessionId = 0x0FE0E112u;

// The socket seam: counts what the role ships, receives nothing.
class CountingSocket final : public opennova::IDatagramSocket {
public:
	int sends = 0;
	int recv_from(uint8_t *, std::size_t, PeerAddr &) override { return 0; }
	void send_to(const PeerAddr &, const uint8_t *, std::size_t) override { ++sends; }
};

// The kernel lives on the heap: a World-carrying frame is megabytes.
struct Harness {
	std::unique_ptr<mission::MissionKernel> kernel = std::make_unique<mission::MissionKernel>();
	inmatch::JoinerRole role;
	CountingSocket socket;
	inmatch::TickInput input;
	std::vector<std::string> seams;

	Harness() {
		kernel->world.registry.configure_pool(0, 16);
		role.bind(*kernel);
		role.set_socket(&socket, PeerAddr{});
		role.create_runtime("BridgeJoiner", inmatch::JoinRole::Player, "");
		role.kit_seams.apply_authoritative = [this] { seams.push_back("loadout"); };
		role.kit_seams.reseed_on_side_change = [this] {
			seams.push_back("reseed");
			return false;
		};
		role.kit_seams.push = [this] { seams.push_back("push_kit"); };
		role.kit_seams.respawn = [this] { seams.push_back("respawn"); };
	}
	w::LocalPlayerWeapon &weapon() { return kernel->local.weapon; }
	w::WeaponInventory &inventory() { return kernel->local.inventory; }
};

// A pre-match frame ships the one-shot ClientHello, runs the loadout seams in
// the witnessed order, advances the clock, and never spawns L.
bool run_pre_match_frame() {
	Harness h;
	h.role.run_tick(h.input);
	const std::vector<std::string> expected = {"loadout", "reseed"};
	if (!expect(h.seams == expected, "pre-match frame: the loadout seams in order")) {
		for (const std::string &c : h.seams) std::fprintf(stderr, "  %s\n", c.c_str());
		return false;
	}
	if (!expect(h.socket.sends == 1, "pre-match frame: the ClientHello shipped once")) return false;
	if (!expect(h.role.started(), "pre-match frame: the hello latch armed")) return false;
	if (!expect(!h.role.local_spawned(), "pre-match frame: no L")) return false;
	if (!expect(!h.kernel->world.cached.local_player.valid(), "pre-match frame: no local player")) return false;
	if (!expect(h.role.now_tick() == 1, "pre-match frame: the clock advanced")) return false;
	if (!expect(h.role.take_diagnostic_sample(), "pre-match frame: the tripwire sampled on tick 0")) return false;

	// Frame 2: no second hello, no tripwire sample (1 % 62 != 0), still no L.
	h.seams.clear();
	h.role.run_tick(h.input);
	if (!expect(h.seams == expected, "frame 2: the seams again")) return false;
	if (!expect(h.socket.sends == 1, "frame 2: hello is a one-shot")) return false;
	if (!expect(!h.role.take_diagnostic_sample(), "frame 2: no tripwire sample")) return false;
	return expect(h.role.now_tick() == 2, "frame 2: the clock advanced again");
}

// The preload frame (no world to tick) shares the hello latch and the clock.
bool run_preload_frame() {
	Harness h;
	h.role.poll_preload();
	if (!expect(h.socket.sends == 1 && h.role.started(), "preload: the hello shipped once")) return false;
	if (!expect(h.role.now_tick() == 1, "preload: the clock advanced")) return false;
	h.role.poll_preload();
	if (!expect(h.socket.sends == 1, "preload: hello is a one-shot")) return false;
	return expect(h.role.now_tick() == 2, "preload: the clock advanced again");
}

// A seeded in-match runtime spawns L on the first frame (the in-match edge),
// latches H, clears the join-wait latches, and seeds the look heading.
bool run_in_match_spawn_edge() {
	Harness h;
	// Production order: the pre-load preload frame ships the ClientHello (the
	// latch arms there), the session establishes later. runtime.start()
	// RESETS a session, so the latch must already be armed when the in-match
	// frame runs — exactly what poll_preload guarantees.
	h.role.poll_preload();
	if (!expect(h.role.started() && h.socket.sends == 1,
			"in-match: the preload hello armed the latch")) return false;
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
	if (!expect(h.role.runtime->in_match(), "in-match: seeded runtime is InMatch")) return false;
	h.weapon().fire_held = true;
	h.weapon().fire_pressed = true;
	h.weapon().reload_pressed = true;

	h.role.run_tick(h.input);
	if (!expect(h.role.local_spawned(), "in-match frame: L spawned on the edge")) return false;
	if (!expect(h.socket.sends == 1, "in-match frame: no second hello on the seeded session")) return false;
	if (!expect(h.kernel->world.cached.local_player.valid(),
			"in-match frame: cached.local_player published")) return false;
	if (!expect(h.role.self_wire_handle() == 0x0005,
			"in-match frame: H latched from the runtime")) return false;
	// The join-wait fire latches died at the spawn edge (the weapon pump ran
	// gated open the same frame over the cleared latches).
	if (!expect(!h.weapon().fire_held && !h.weapon().fire_pressed &&
					!h.weapon().reload_pressed,
			"in-match frame: join-wait weapon latches cleared")) return false;

	// Frame 2: the edge is a one-shot.
	const w::EntityHandle L = h.kernel->world.cached.local_player;
	h.role.run_tick(h.input);
	if (!expect(h.role.local_spawned(), "frame 2: still spawned")) return false;
	return expect(h.kernel->world.cached.local_player == L, "frame 2: no re-spawn");
}

// reset_for_join re-arms the per-session latches for a fresh dial.
bool run_reset_for_join() {
	Harness h;
	h.role.run_tick(h.input);
	if (!expect(h.role.started(), "reset: latch armed before")) return false;
	h.role.reset_for_join();
	if (!expect(!h.role.started() && !h.role.local_spawned() &&
					h.role.self_wire_handle() == 0 &&
					h.role.flat_seconds() == 0 &&
					!h.role.freeze_suspected(),
			"reset: per-session latches cleared")) return false;
	// The clock deliberately survives (retail's per-frame tick is process-scoped).
	return expect(h.role.now_tick() == 1, "reset: the clock is not a session latch");
}

// The D-NET-194 wire-header joiner: no pre-load kit, so inventory_valid is
// false at frame entry. The first S2C 0x5A grant folds and the
// apply_authoritative seam flips the live flag + arms the equipped slot
// INSIDE run_client_net_frame — i.e. BEFORE spawn_and_arm the SAME frame. The
// spawn edge must see that live flip and stamp L's equipped_adm_index;
// otherwise the entity carries the default adm on the C2S 0x0C uplink until
// the next respawn.
bool run_spawn_stamps_equipped_adm_from_midframe_grant() {
	Harness h;
	// Model retail's mid-frame apply: flip the live inventory-valid flag and
	// arm the equipped combo, exactly as apply_authoritative_loadout ->
	// local_loadout_rebuild does before the spawn block runs.
	constexpr int16_t kGrantedAdm = 16; // WPN_M16BURST-shaped grant
	h.role.kit_seams.apply_authoritative = [&h, kGrantedAdm] {
		h.seams.push_back("loadout");
		h.kernel->local.inventory_valid = true;
		h.inventory().equipped_combo = 0;
		h.inventory().slots[0].adm_index = kGrantedAdm;
	};
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
	if (!expect(h.role.runtime->in_match(), "midframe grant: seeded runtime InMatch"))
		return false;

	h.role.run_tick(h.input);
	if (!expect(h.role.local_spawned(), "midframe grant: L spawned")) return false;
	const w::Entity *L = h.kernel->world.registry.get(h.kernel->world.cached.local_player);
	if (!expect(L != nullptr, "midframe grant: L resolvable")) return false;
	return expect(L->equipped_adm_index == kGrantedAdm,
			"midframe grant: the same-frame grant stamped L's equipped adm "
			"(regression: a by-value inventory_valid froze it at frame entry)");
}

} // namespace

int main() {
	bool ok = true;
	ok &= run_pre_match_frame();
	ok &= run_preload_frame();
	ok &= run_in_match_spawn_edge();
	ok &= run_spawn_stamps_equipped_adm_from_midframe_grant();
	ok &= run_reset_for_join();
	if (!ok) return 1;
	std::printf("joiner_role_test: OK\n");
	return 0;
}
