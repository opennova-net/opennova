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
//
// What is NOT folded in yet: S2C 0x0A per-frame compact records (§5.9). They
// carry the host's view of every nearby entity, but their positions are
// wire-COMPRESSED against an anchor whose reconstruction is not yet
// IDA-witnessed (faithful-port: we don't invent the decompression). When that
// lands, those samples extend every entity's track here — the struct already
// accommodates it via ReplaySampleSource.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "novaworld/ingame_decode.h"
#include "novaworld/wire_capture.h"

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
	bool has_spawn = false;
	ReplaySample spawn;      // authoritative initial WORLD pose
	std::vector<ReplaySample> track; // time-ordered samples (seeded with spawn)
};

struct ReplayTimeline {
	int first_frame = 0;     // capture frame span (the natural scrubber range)
	int last_frame = 0;
	std::vector<ReplayEntity> entities; // in spawn/first-seen order
};

// Assemble the timeline from the decoded in-game message stream
// (decode_capture_to_messages). Pure data — no I/O, no formatting.
//
// `class_of` maps a wire type_id to its §5.10b compact dispatch class (built from
// items.def by the caller); it is required to walk the S2C 0x0A per-frame records
// (their width is class-dependent). When empty, 0x0A motion is skipped and the
// timeline carries only spawns + C2S 0x0C uplinks. Mounted 0x0A records (whose
// positions are vehicle-local) are skipped until the parent transform lands.
ReplayTimeline build_replay_timeline(
    const std::vector<InGameMessage> &messages,
    const std::function<EntityClass(uint16_t)> &class_of = {});

} // namespace opennova
