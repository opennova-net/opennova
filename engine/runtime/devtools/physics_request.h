// A typed contact-capture mutation the F3 Physics window queues for its
// embedder to drain (ADR 0042 d6; the RaysRequest pattern). The window never
// mutates engine state itself — the embedder routes the mask/clear requests
// into the Simulation's contact-debug seam. The capture itself arms and
// disarms with the window's visibility (the Rays window's rule), so there is
// no capture request.
#pragma once

#include <cstdint>

namespace opennova::devtools {

struct PhysicsRequest {
	enum class Kind {
		SetKindMask, // a = the new draw mask (bit i = contact kind i)
		Clear,       // zero the ring and every lifetime counter
	};

	Kind kind = Kind::SetKindMask;
	int32_t a = 0;
};

}  // namespace opennova::devtools
