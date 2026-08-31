// The typed entity-directory record the embedder pushes into the F3 Entities
// window (ADR 0042 d6: records in, typed requests out). A plain value — the
// engine join (world::inspect::entity_directory) computed by the embedder on
// its refresh cadence, snapshotted here so the window never reaches into a
// live World. An invalid snapshot clears the window (the world unloaded).
#pragma once

#include <cstdint>
#include <vector>

#include <runtime/world/inspect.h>

namespace opennova::devtools {

struct EntityDirectorySnapshot {
	std::vector<world::inspect::EntityRow> rows;
	bool valid = false;
	// world.logic_tick at the join, so the window can label how fresh the
	// reading is (consumers track deltas, not absolutes).
	uint64_t logic_tick = 0;
	// This peer owns the world (single player or the session host; the
	// inmatch role, ADR 0042 d5): the entity edits are enabled only then. A
	// joiner reads everything and edits nothing (its rows are replicas the
	// wire re-writes, and a local-player teleport would ride the uplink).
	bool authority = false;
	// A wire session is live under this peer's authority (a listen or
	// dedicated host): edits that change what the wire carries (the AIData
	// gate of the 0x0D record) stay refused for a stock client's sake.
	bool session_live = false;
};

}  // namespace opennova::devtools
