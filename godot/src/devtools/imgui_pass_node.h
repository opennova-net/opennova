#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <cstdint>

namespace opennova::devtools {
class ImGuiPass;
}

namespace godot {

// The ImGui pass's Godot seam (ADR 0039): the one node per product that
// supplies what only the shell has — the Dear ImGui context the imgui-godot
// addon created (handed to the engine once, ImGuiGD.GetImGuiPtrs ->
// ImGuiPass::attach_imgui) and the per-frame layout bracket (a _process just
// under the addon's own render pass, after every game callback of the frame).
// A product subclass (DevTools for the game, OnedUi for ONED) owns the engine
// pass and its window set and reports it through engine_pass().
//
// Attachment only governs drawing: the editor and headless runs (the
// DisplayServer "headless") never attach, and a missing addon warns once and
// leaves the node inert; the pass's open state and every product seam keep
// working so tests can pin them.
class ImGuiPassNode : public Node {
	GDCLASS(ImGuiPassNode, Node)

public:
	void _ready() override;
	void _exit_tree() override;
	void _process(double p_delta) override;

	// True while an ImGui context is attached and the pass draws.
	bool is_available() const { return attached_; }

protected:
	static void _bind_methods();

	// The engine pass this node drives; nullptr when the product compiled it out.
	virtual opennova::devtools::ImGuiPass *engine_pass() { return nullptr; }
	// After each layout pass: what the pass drew this frame and how long the
	// engine's layout took.
	virtual void after_layout(uint64_t p_frame_index, bool p_drew, int64_t p_layout_us) {
		(void)p_frame_index;
		(void)p_drew;
		(void)p_layout_us;
	}
	// The addon's layer follows the pass's open state (input and rendering
	// stop while closed); products call this on their own open edges.
	void sync_layer_visible();

private:
	bool attach(opennova::devtools::ImGuiPass &p_pass);
	void set_layer_visible(bool p_visible);

	bool attached_ = false;
	bool layer_visible_ = false;
};

} // namespace godot
