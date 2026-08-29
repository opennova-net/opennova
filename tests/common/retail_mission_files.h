// Retail path glue for the asset-gated world/mission ctests (ADR 0042 d3):
// the engine's own mission::MissionKernel is the boot + state + no-net tick
// (the rig body was promoted there), inmatch::listen_host is the SP listen
// half, and this pair keeps only what a ctest needs on top — the mission's
// .cpt/.trn terrain documents (the format-typed leg on the far side of the
// ADR 0020 seam), the listen/no-net tick dispatch, and the inmatch::TickTarget
// adapter the Session-driven tests bank on.
//
// Gating is the caller's: open() (inherited from the kernel) takes the root
// from retail::install() / retail::assets() and reports a missing file so the
// test can retail::skip.
#ifndef OPENNOVA_TEST_RETAIL_MISSION_FILES_H
#define OPENNOVA_TEST_RETAIL_MISSION_FILES_H

#include <base/io/bam.h>
#include <net/inmatch/listen_host.h>
#include <net/inmatch/session.h>
#include <runtime/mission/mission_kernel.h>

#include <cmath>
#include <cstdint>
#include <string>

namespace opennova::testrig {

struct BootOptions {
	bool playable = true;   // spawn the host's own player after load
	bool wac = true;        // game.wac / server.wac / <mission>.wac when present
	bool terrain = true;    // the mission's .cpt/.trn height field
	bool collision = true;
	bool seat_specs = true; // the native seat/mount table (S16); off = the bare promote
	// The SP listen server (ADR 0009/0012): the npruntime host session over an
	// in-process loopback, whose Server_TickUpdate owns the logic tick, the
	// WAC 'humans' gate and the SP kill tallies. Off = the bare no-net world
	// tick (what the AI-path and convoy tests drive).
	bool listen_server = true;
	std::string infantry_adm = mission::kDefaultInfantryAdm;
};

// The 62.5 Hz logic-tick clock in mission seconds.
constexpr double kTickSeconds = 1.0 / 62.5;
inline int ticks_for_seconds(double seconds) {
	return static_cast<int>(seconds / kTickSeconds + 0.5);
}

constexpr double kBamPerRad = opennova::io::kBamPerRadian;
constexpr double kBamPerDeg = 4294967296.0 / 360.0;

inline int32_t bam_from_radians(double radians) {
	return static_cast<int32_t>(static_cast<int64_t>(std::llround(radians * kBamPerRad)));
}

// The kernel plus the ctest-side halves: retail terrain documents, the SP
// listen state, and the TickTarget adapter. Everything a test reads or
// mutates — world, ai, input, weapon, loadout, the player/entity/terrain/
// observation seams — is the kernel's own public surface.
class RetailMissionRig : public mission::MissionKernel, public inmatch::TickTarget {
public:
	RetailMissionRig();

	// The S9 boot over the opened mission: the terrain load first (the
	// engine's one cpt/trn(+charmap) loader into the kernel's store), then the
	// kernel boot with the listen bring-up hook when listen_server is on.
	bool boot(const BootOptions &options, std::string &error);

	// One authoritative logic tick: the listen frame
	// (inmatch::listen_host::frame — Server_TickUpdate's owner pump between
	// the local pumps) or, with listen_server off, the kernel's bare no-net
	// tick.
	void tick();
	void tick(int count);

	// --- the SP listen server (npruntime) ------------------------------------
	bool listen_server = false;
	inmatch::ListenHostState host;

	// inmatch::TickTarget
	inmatch::TickOutcome advance_mission_tick(const inmatch::TickInput &input) override;
	bool reset_mission_to_baseline(inmatch::SessionError &error) override;
	void close_mission() override;

private:
	bool load_terrain(std::string &error);
};

// Mission-space helpers.
inline float planar_distance(const world::Vec3 &a, const world::Vec3 &b) {
	const float dx = a.x - b.x, dy = a.y - b.y;
	return std::sqrt(dx * dx + dy * dy);
}
inline float distance(const world::Vec3 &a, const world::Vec3 &b) {
	const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}
inline world::Vec3 ai_position(const world::AiEntity &e) {
	return world::Vec3{e.pos[0] / 65536.0f, e.pos[1] / 65536.0f, e.pos[2] / 65536.0f};
}

} // namespace opennova::testrig

#endif // OPENNOVA_TEST_RETAIL_MISSION_FILES_H
