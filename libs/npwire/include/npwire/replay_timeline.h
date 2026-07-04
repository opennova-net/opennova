#pragma once

// Replay timeline — assemble a capture's in-game messages into per-entity
// position tracks over time.
//
// This is the reusable foundation the capture visualizers (the standalone 2D
// viewer; a future ONED 3D "Net Replay" workspace) and, eventually, a real MP
// client all sit on: turn the wire stream into "what entity is where, when".
//
// What feeds a track:
//   - Pool spawns (S2C 0x0D items/vehicles §5.11, S2C 0x20 markers §5.12, S2C
//     0x0C organics §5.23) populate the static entity layout: type, name, team,
//     and the authoritative initial WORLD pose (i32 16.16).
//   - C2S 0x0C extended uplinks (§5.10) append per-frame motion for the joiner's
//     OWN player — full i32 16.16 positions, the cleanest moving track in a
//     loopback capture.
//   - S2C 0x0A per-frame compact records (§5.9) append the host's view of every
//     nearby entity's motion (positions decompressed via
//     network_decompress_fixedpoint + the message anchor). On-foot records land in
//     world directly; mounted (vehicle-local) records are lifted into world by the
//     parent-vehicle transform (network_transform_local_to_world), so a rider
//     follows its vehicle / weapon mount instead of freezing.
//
// Beyond per-entity tracks, the timeline also carries an EVENT stream and an
// ENVIRONMENT stream decoded from the same wire: weapon-fire (C2S 0x06),
// weapon-hit (S2C 0x0A tag==2), kills + game events (S2C 0x1E / 0x26),
// capture-zone state (S2C 0x40), and the 0x0A env snapshot. These let a viewer
// render a kill feed, projectiles, and an environment readout.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "npwire/ingame_decode.h"
#include "npwire/wire_capture.h"

namespace opennova {

enum class ReplaySampleSource : uint8_t {
	Spawn = 0,        // initial pose from a pool spawn — WORLD frame (16.16)
	ClientUplink = 1, // C2S 0x0C extended uplink — WORLD+origin frame (16.16)
	FrameUpdate = 2,  // S2C 0x0A compact record — decompressed + header anchor
};

// One position/heading sample for an entity at a capture frame.
struct ReplaySample {
	int frame_index = 0;             // capture order the sample was witnessed at
	int32_t x = 0, y = 0, z = 0;     // i32 16.16 fixed-point
	double heading_deg = 0.0;        // [0,360); valid iff has_heading
	bool has_heading = false;
	ReplaySampleSource source = ReplaySampleSource::Spawn;
	bool dead = false;               // entity in dead/spectator state this sample
	                                 // (S2C 0x0A compact flags & 0x02 — the dead/
	                                 // spectator bit the read path gates on)
	bool respawn = false;            // this sample is a respawn snap: a dead->alive
	                                 // teleport — interp/trails must NOT bridge to it
	bool mounted = false;            // position came from the parent-vehicle transform
	                                 // (rider on a vehicle / weapon mount), not the wire
};

// One entity reconstructed from the capture, keyed by its network handle.
struct ReplayEntity {
	uint16_t handle = 0;     // (pool << 12) | slot
	uint8_t  pool = 0;       // handle >> 12 (0 organic, 1 item, 2 building, 3 marker)
	uint16_t type_id = 0;    // wire itemTypeId (items.def id - 100000)
	std::string name;        // entity name when the spawn carried one
	int  team = 0;           // BMS team (1=Blue, 2=Red); valid iff team_known
	bool team_known = false;
	uint16_t net_id = 0xFFFF;// organic net_id / marker authored bms id, when known
	char spawn_tag = 0;      // 0x0D / 0x20 / 0x0C the entity entered on (0 = uplink-only)
	int  owner_session = 0;  // client session whose C2S 0x0C uplink owns this entity's
	                         // clean track (0 = host-owned / no uplink owner)
	bool has_spawn = false;
	ReplaySample spawn;      // authoritative initial WORLD pose
	std::vector<ReplaySample> track; // time-ordered samples (seeded with spawn)
};

// A discrete in-game event, decoded from the wire, placed on the timeline.
enum class ReplayEventKind : uint8_t {
	Fire = 0,     // C2S 0x06 weapon-fire (world origin + direction) §5.16
	Hit,          // S2C 0x0A tag==2 validated weapon-hit (impact world pos) §5.9.1
	Kill,         // S2C 0x1E kill-type event / S2C 0x26 — an entity death
	GameEvent,    // S2C 0x1E non-kill (objective / zone / misc canned message)
	CaptureZone,  // S2C 0x40 minimap capture-zone state change §5.19
};

struct ReplayEvent {
	int frame_index = 0;
	ReplayEventKind kind = ReplayEventKind::Fire;
	bool has_pos = false;
	int32_t x = 0, y = 0, z = 0;   // world 16.16, valid iff has_pos
	bool has_dir = false;
	int32_t dir_x = 0, dir_y = 0;  // fire direction (raw i32 wire), valid iff has_dir
	uint16_t source = 0xFFFF;      // shooter / attacker / killer handle (0xFFFF=none)
	uint16_t target = 0xFFFF;      // target / victim handle (0xFFFF=none)
	uint16_t aux = 0xFFFF;         // weapon handle (hit) / aux actor (game event) / flags (zone)
	uint8_t  adm_index = 0;        // weapon/action descriptor (Fire/Hit)
	uint8_t  event_type = 0;       // 0x1E event_type (Kill/GameEvent) / 0x40 icon color (Zone)
	bool     sound = false;        // the engine plays a sound for this event (fire/hit/announce)
	std::string label;             // best-effort tag: STRCND key / category
};

// One environment snapshot from the S2C 0x0A header sub-block (case 2).
struct ReplayEnvSample {
	int frame_index = 0;
	uint16_t fog_dist = 0, fog_accel = 0, tod_fixed = 0;
	uint8_t  quake_ticks = 0, cloud_scroll = 0, overcast = 0;
};

struct ReplayTimeline {
	int first_frame = 0;     // capture frame span (the natural scrubber range)
	int last_frame = 0;
	std::vector<ReplayEntity> entities;     // in spawn/first-seen order
	std::vector<ReplayEvent>  events;        // fire / hit / kill / game-event / zone
	std::vector<ReplayEnvSample> environment; // 0x0A env snapshots over time
};

// Assemble the timeline from the decoded in-game message stream
// (decode_capture_to_messages). Pure data — no I/O, no formatting.
//
// `class_of` maps a wire type_id to its §5.10b compact dispatch class (built from
// items.def by the caller); it is required to walk the S2C 0x0A per-frame records
// (their width is class-dependent). When empty, 0x0A motion is skipped and the
// timeline carries only spawns + C2S 0x0C uplinks. Mounted 0x0A records (whose
// positions are vehicle-local) are lifted into world via the parent's pose
// (network_transform_local_to_world), resolved bottom-up so nested mounts — a rider
// on a weapon mount on a vehicle — place correctly.
ReplayTimeline build_replay_timeline(
    const std::vector<InGameMessage> &messages,
    const std::function<EntityClass(uint16_t)> &class_of = {});

// ===========================================================================
// Per-participant world model (the client/host split). Each network participant
// — the host (authority, sends S2C) + one client per session (keyed by its UDP
// port) — reconstructs its OWN view: its own player from its clean C2S 0x0C
// uplink, every other entity from the S2C 0x0A it received. Diffing the host's
// broadcast view against a client's reconstruction validates the decompress +
// anchor decode under motion. The SAME structs feed the live Godot runtime from
// the socket — this is the runtime world model, not replay-only.
// ===========================================================================

struct Participant {
	int id = 0;          // 0 = host; 1..N = clients in roster order
	bool is_host = false;
	int session = 0;     // client-side UDP port (0 for the host)
	std::string name;    // "host" / "client <port>" (later: NWHANDLE from the roster)
};

struct ParticipantView {
	Participant who;
	ReplayTimeline timeline; // projected to this participant's vantage
};

// Reconstruct every participant's world from a capture: decode once, build the
// single timeline once, then project per participant by ReplaySample.source +
// entity ownership. Index 0 is the host view; then one view per client session.
std::vector<ParticipantView> build_per_participant_world(
    const std::vector<CaptureDatagram> &datagrams,
    const std::function<EntityClass(uint16_t)> &class_of = {});

// Per-entity position divergence between two participant views (Step 3 harness).
struct EntityDivergence {
	uint16_t handle = 0;
	int compared = 0;      // overlapping frames compared
	double max_dist = 0.0; // worst per-frame position error (meters)
	double mean_dist = 0.0;
};

struct ViewDiff {
	std::vector<EntityDivergence> entities; // only entities that actually diverge
};

// Compare two views entity-by-entity (by handle), interpolating positions at each
// other's sample frames. Surfaces where the host broadcast and a client's
// reconstruction disagree — the RE-validation signal.
ViewDiff diff_participant_views(const ParticipantView &a, const ParticipantView &b);

} // namespace opennova
