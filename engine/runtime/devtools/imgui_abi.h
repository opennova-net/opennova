// The Dear ImGui ABI fingerprint of the engine's ImGui copy (ADR 0039).
//
// The imgui-godot addon owns the ImGuiContext; before handing its pointer over
// (ImGuiGD.GetImGuiPtrs) it compares the caller's IMGUI_VERSION and the sizes
// of ImGuiIO, ImDrawVert, ImDrawIdx and ImWchar against its own build, and
// refuses a mismatch. This header is the Godot-free way for the shell to read
// those five values off the engine copy: it deliberately includes no ImGui
// header, so godot/src never sees one.
#pragma once

namespace opennova::devtools {

struct ImGuiAbi {
	const char *version;  // IMGUI_VERSION, e.g. "1.91.6"
	int io_size;          // sizeof(ImGuiIO)
	int vert_size;        // sizeof(ImDrawVert)
	int idx_size;         // sizeof(ImDrawIdx)
	int wchar_size;       // sizeof(ImWchar)
};

ImGuiAbi imgui_abi();

}  // namespace opennova::devtools
