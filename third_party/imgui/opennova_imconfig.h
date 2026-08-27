// Dear ImGui compile-time config for the engine's copy (IMGUI_USER_CONFIG).
//
// The ImGui context is created by the imgui-godot addon's own ImGui build and
// shared with this copy (ImGui::SetCurrentContext + SetAllocatorFunctions), so
// every option that changes a struct layout must match the addon's
// imconfig-godot.h. At v1.91.6-docking that is exactly one define:
// IMGUI_DISABLE_OBSOLETE_FUNCTIONS guards three ImGuiIO data members
// (GetClipboardTextFn, SetClipboardTextFn, ClipboardUserData). The addon's
// remaining settings (IM_VEC2_CLASS_EXTRA / IM_VEC4_CLASS_EXTRA Godot
// conversions, FreeType, C++20) do not affect the fingerprinted layouts, and
// the engine stays Godot-free, so nothing else is mirrored here.
//
// ImGuiGD.GetImGuiPtrs re-checks IMGUI_VERSION and sizeof(ImGuiIO/ImDrawVert/
// ImDrawIdx/ImWchar) at runtime; tests/devtools/devtools_test.cpp pins them at
// build time.
#pragma once

#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS
