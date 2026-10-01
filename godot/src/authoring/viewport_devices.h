#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>

#include <functional>
#include <memory>

#include <editor/preview/viewport_kinds.h>

#include "authoring/viewport_applier.h"

namespace opennova::editor {
class ViewportDevice;
struct EditorRequest;
} // namespace opennova::editor

namespace godot {

// What a device hands the requests it makes to (a Control device's edits: the script device's spans,
// S13 V10); the Shell serves each at once (EditorApp).
using ViewportDeviceRequests = std::function<void(const opennova::editor::EditorRequest &)>;

// A viewport kind's device (ADR 0046 S13 V5, V10): one row per ViewportKind, in its order, in
// authoring/viewport_devices.cpp, which does not build without it (static_asserts, as the kinds'
// and the views' tables): what makes its device, one of two flavours. A SubViewport device over the
// kind's applier (`make`: its runtime nodes under the device's SubViewport, the menu's MenuFrame, the
// model's ObjectModel with its camera and light), or a Control device of its own (`make_control`: a
// Godot Control placed over the rect the canvas reserves, which owns the input there, the script
// device's CodeEdit, S13 V10).
struct ViewportDeviceRow {
	opennova::editor::ViewportKind kind = opennova::editor::ViewportKind::kCount;
	std::unique_ptr<ViewportApplier> (*make)(SubViewport &viewport) = nullptr;
	std::unique_ptr<opennova::editor::ViewportDevice> (*make_control)(Node &owner, ViewportDeviceRequests requests) =
			nullptr;
};

// A kind's row; null past the last kind.
const ViewportDeviceRow *viewport_device_row(opennova::editor::ViewportKind kind);
// A new device of `kind` (EditorApp's device cache's factory, ViewportDeviceCache), its nodes children
// of `owner`: a SubViewport device's SubViewport handed to `retire` when the device goes (the Shell
// frees it at its next frame), a Control device's requests to `requests`; null past the last kind.
std::unique_ptr<opennova::editor::ViewportDevice> make_viewport_device(Node &owner, opennova::editor::ViewportKind kind,
		std::function<void(SubViewport *)> retire, ViewportDeviceRequests requests);

} // namespace godot
