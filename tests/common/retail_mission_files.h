// Retail path glue for the asset-gated world/mission ctests (ADR 0042 d3):
// the engine's own mission::MissionKernel is the boot + state, the inmatch
// roles (LocalRole / HostRole, ADR 0043 d3) are the ticks, and this pair keeps
// only what a ctest needs on top — the listen/no-net role pick the
// Session-driven tests bank on (the kernel boot loads the terrain field
// through the mounted root itself).
//
// Gating is the caller's: open() (inherited from the kernel) takes the root
// from retail::install() / retail::assets() and reports a missing file so the
// test can retail::skip.
#pragma once

#include <base/io/bam.h>
#include <runtime/inmatch/host_role.h>
#include <runtime/inmatch/local_role.h>
#include <runtime/inmatch/session.h>
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

using opennova::io::bam_from_radians;

// The kernel plus the ctest-side half: the two tick roles. Everything a test reads or
// mutates — world, ai, input, weapon, loadout, the player/entity/terrain/
// observation seams — is the kernel's own public surface.
class RetailMissionRig : public mission::MissionKernel {
public:
	RetailMissionRig();

	// The S9 boot over the opened mission (the kernel loads the terrain field
	// through the mounted root itself), with the listen bring-up hook when
	// listen_server is on.
	bool boot(const BootOptions &options, std::string &error);

	// One frame of one authoritative logic tick through the active role: the
	// host role's listen frame (Server_TickUpdate's owner pump between the
	// local pumps) or, with listen_server off, the local role's bare no-net
	// tick; then the frame's render-side cine pass.
	void tick();
	void tick(int count);

	// --- the SP listen server (npruntime) ------------------------------------
	bool listen_server = false;
	// The two roles a rig can run its ticks through (ADR 0043 d3); `host`
	// aliases the host role's session state for the tests that read it.
	inmatch::LocalRole local_role;
	inmatch::HostRole host_role;
	inmatch::ListenHostState &host = host_role.state;
	inmatch::Role &role() { return listen_server ? static_cast<inmatch::Role &>(host_role) : local_role; }
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
