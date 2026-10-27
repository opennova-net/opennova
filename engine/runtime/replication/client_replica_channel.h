#pragma once

#include <runtime/replication/client_state.h>

// Internal (src-local) seam: a decoded row's AnimMap primary channel
// (client_replica_channel.cpp), for the body passes in client_replica_motion.cpp.

namespace opennova::world {
class IRootMotionSource;
class AnimVariantRings;
struct RootMotionFrame;
} // namespace opennova::world

namespace opennova::replication {

// The channel's state half: the gait insert, the re-init onto `target_state` and the deferred
// promotion; true when the promotion released a loop parked on its boundary (the contract in
// the TU).
bool row_channel_prepare(ClientEntityState &es, world::IRootMotionSource &src,
                         world::AnimVariantRings *rings, int &target_state);

// The channel's playhead half: the advance and an unarmed loop wrap's ring serve, or the
// blend's two halves; false when the state has no track (the contract in the TU).
bool row_channel_advance(ClientEntityState &es, world::IRootMotionSource &src,
                         world::AnimVariantRings *rings, bool released,
                         world::RootMotionFrame &frame);

} // namespace opennova::replication
