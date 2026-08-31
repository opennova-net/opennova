// A typed contact-capture mutation the F3 Physics window queues for its
// embedder to drain (ADR 0042 d6; the RaysRequest pattern). The window never
// mutates engine state itself — the embedder routes the mask/clear/capture
// requests into the Simulation's contact-debug seam, and the view toggle out
// to the shell (the GDScript debug-view set owns building the collision view).
#pragma once

#include <cstdint>

namespace opennova::devtools {

struct PhysicsRequest {
	enum class Kind {
		SetKindMask,       // a = the new draw mask (bit i = contact kind i)
		Clear,             // zero the ring and every lifetime counter
		SetViewShown,      // a != 0: build the collision view (show_collision), 0: free it
		SetCaptureEnabled, // a != 0: arm the contact ring, 0: disarm and free it
	};

	Kind kind = Kind::SetKindMask;
	int32_t a = 0;
};

}  // namespace opennova::devtools
