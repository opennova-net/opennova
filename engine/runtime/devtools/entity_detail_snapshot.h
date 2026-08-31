// The typed per-entity detail record the embedder pushes into the F3 Entities
// window for its selected row (ADR 0042 d6: records in, typed requests out).
// A plain value: the engine card (world::inspect::build_entity_card) computed
// by the embedder whenever the selection changes and on the directory's
// refresh cadence, snapshotted here so the window never reaches into a live
// World. An invalid card clears the pane; card.handle is the identity the
// window checks against its selection (a late push for a previous selection
// is dropped).
#pragma once

#include <cstdint>

#include <runtime/world/inspect.h>

namespace opennova::devtools {

struct EntityDetailSnapshot {
	world::inspect::EntityCard card;
	// world.logic_tick at the build, so the pane can label how fresh the
	// reading is (consumers track deltas, not absolutes).
	uint64_t logic_tick = 0;
};

}  // namespace opennova::devtools
