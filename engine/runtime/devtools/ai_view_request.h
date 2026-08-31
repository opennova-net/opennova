// The AI window's typed request: one overlay-view toggle (ADR 0042 d6 —
// window mutations leave as typed requests). The target is the shell's
// world-parented AI debug view, so the embedder drains these into a device
// seam rather than the engine command layer; the flipped state comes back as
// pushed truth in the next AiDebugSnapshot's overlay block.
#pragma once

namespace opennova::devtools {

struct AiViewRequest {
	enum class Element {
		Master, // build/free the overlay view itself
		Labels,
		Routes,
		Targets,
		Rings,
	};
	Element element = Element::Master;
	bool enabled = false;
};

}  // namespace opennova::devtools
