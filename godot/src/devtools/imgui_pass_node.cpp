#include "devtools/imgui_pass_node.h"

#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <runtime/devtools/imgui_abi.h>
#include <runtime/devtools/imgui_pass.h>

#include <climits>

namespace godot {

namespace {

constexpr const char *kImGuiSingleton = "ImGuiGD";

} // namespace

void ImGuiPassNode::_bind_methods() {
	ClassDB::bind_method(D_METHOD("is_available"), &ImGuiPassNode::is_available);
}

bool ImGuiPassNode::attach(opennova::devtools::ImGuiPass &p_pass) {
	Engine *engine = Engine::get_singleton();
	if (engine->is_editor_hint()) {
		return false;
	}
	DisplayServer *display = DisplayServer::get_singleton();
	if (display == nullptr || display->get_name() == "headless") {
		return false;
	}
	if (!engine->has_singleton(kImGuiSingleton)) {
		UtilityFunctions::push_warning(
				"ImGuiPassNode: the imgui-godot addon is not loaded (run scripts/bootstrap_godot.sh); ImGui surfaces unavailable");
		return false;
	}
	Object *imgui = engine->get_singleton(kImGuiSingleton);
	const opennova::devtools::ImGuiAbi abi = opennova::devtools::imgui_abi();
	const Variant pointers = imgui->call("GetImGuiPtrs", String::utf8(abi.version), abi.io_size,
			abi.vert_size, abi.idx_size, abi.wchar_size);
	if (pointers.get_type() != Variant::PACKED_INT64_ARRAY) {
		UtilityFunctions::push_warning("ImGuiPassNode: ImGuiGD.GetImGuiPtrs returned no pointer table; ImGui surfaces unavailable");
		return false;
	}
	const PackedInt64Array table = pointers;
	if (table.size() != 3 || table[0] == 0) {
		// The addon printed the version/size mismatch itself.
		UtilityFunctions::push_warning(vformat(
				"ImGuiPassNode: the imgui-godot addon rejected the engine's ImGui %s (context hand-off refused); ImGui surfaces unavailable",
				abi.version));
		return false;
	}
	return p_pass.attach_imgui(reinterpret_cast<void *>(static_cast<intptr_t>(table[0])),
			reinterpret_cast<opennova::devtools::ImGuiAllocFn>(static_cast<intptr_t>(table[1])),
			reinterpret_cast<opennova::devtools::ImGuiFreeFn>(static_cast<intptr_t>(table[2])), nullptr);
}

void ImGuiPassNode::set_layer_visible(bool p_visible) {
	Engine *engine = Engine::get_singleton();
	if (!engine->has_singleton(kImGuiSingleton)) {
		return;
	}
	engine->get_singleton(kImGuiSingleton)->call("SetVisible", p_visible);
}

void ImGuiPassNode::sync_layer_visible() {
	opennova::devtools::ImGuiPass *pass = engine_pass();
	const bool visible = attached_ && pass != nullptr && pass->is_open();
	if (visible == layer_visible_) {
		return;
	}
	layer_visible_ = visible;
	set_layer_visible(visible);
}

void ImGuiPassNode::_ready() {
	opennova::devtools::ImGuiPass *pass = engine_pass();
	attached_ = pass != nullptr && attach(*pass);
	if (!attached_) {
		set_process(false);
		return;
	}
	// The addon's helper runs ImGui::NewFrame() at the lowest process priority
	// and its controller renders at the highest; this layout pass sits just
	// under the render, after every game callback of the frame.
	set_process_priority(INT_MAX - 1);
	set_process_mode(PROCESS_MODE_ALWAYS);
	set_process(true);
	sync_layer_visible();
}

void ImGuiPassNode::_exit_tree() {
	if (!attached_) {
		return;
	}
	attached_ = false;
	if (layer_visible_) {
		layer_visible_ = false;
		set_layer_visible(false);
	}
	opennova::devtools::ImGuiPass *pass = engine_pass();
	if (pass != nullptr) {
		pass->detach_imgui();
	}
	set_process(false);
}

void ImGuiPassNode::_process(double p_delta) {
	(void)p_delta;
	opennova::devtools::ImGuiPass *pass = engine_pass();
	if (!attached_ || pass == nullptr) {
		return;
	}
	const uint64_t frame = Engine::get_singleton()->get_process_frames();
	const int64_t t0 = Time::get_singleton()->get_ticks_usec();
	const bool drew = pass->draw_frame(frame);
	after_layout(frame, drew, Time::get_singleton()->get_ticks_usec() - t0);
	sync_layer_visible();
}

} // namespace godot
