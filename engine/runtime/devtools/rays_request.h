// A typed ray-capture mutation the F3 Rays window queues for its embedder to
// drain (ADR 0042 d6; the EnvironmentRequest pattern). The window never
// mutates engine state itself — the embedder routes the filter/clear requests
// into the Simulation's ray-debug seam.
#pragma once

#include <cstdint>

namespace opennova::devtools {

struct RaysRequest {
	enum class Kind {
		SetCategoryMask, // a = the new draw mask (bit i = category i)
		SetTtlTicks,     // a = the new fade window in 62 Hz ticks
		Clear,           // zero every ring and lifetime counter
	};

	Kind kind = Kind::SetCategoryMask;
	int32_t a = 0;
};

}  // namespace opennova::devtools
