#pragma once

#include <cstdint>

namespace opennova {

// Monotonic epoch for engine-side derived caches (decoded textures, GDScript
// mesh/model/graphics caches, ...). NovaResourceRoot bumps it whenever any
// root mounts, rescans, or clears; cache holders remember the epoch they were
// built under and self-clear when it has moved. The epoch is process-global,
// so multiple roots used by tests or authoring tools over-invalidate one
// another — the same blast radius the global texture-resolver clear has
// today, traded for never serving stale assets.
// Starts at 1 so a zero-initialised "epoch I was built at" field always reads
// as stale.
uint64_t cache_epoch();
void bump_cache_epoch();

} // namespace opennova
