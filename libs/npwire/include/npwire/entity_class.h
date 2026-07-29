// The §5.10b per-item replication class, split out of ingame_decode.h so the
// light consumers (netsim state/views, replication_model, the Godot net
// client) stop pulling the full 1,700-line decode surface for one enum.
// ingame_decode.h re-includes this header, so full-surface consumers see no
// change.
#pragma once

#include <cstdint>

namespace opennova {

// §5.10b per-item dispatch class — selects which compact decoder a tag==1 record
// in the S2C 0x0A event loop uses. Seeded from the item's *_function class-tag in
// items.def (ai_function, else move_function) at load time. [orig: ItemDef+356]
enum class EntityClass : uint8_t {
	Unknown = 0,
	Player,   // §5.10  18 B fixed
	Infantry, // §5.14  14 B fixed
	Vehicle,  // §5.13  15 B mounted / 21 B unmounted
	Guided,   // §5.15  variable-length delta codec (deferred)
	NoNetworkCallback, // known ItemDef class with fn[3] == 0; tag==1 header only
};

// Map a 4-char items.def class-tag (case-sensitive §5.10b match) to its class.
EntityClass class_from_tag(const char *tag);

} // namespace opennova
