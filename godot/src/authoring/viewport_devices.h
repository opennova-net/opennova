#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>

#include <functional>
#include <memory>
#include <string>

#include <editor/preview/viewport_kinds.h>

#include "authoring/viewport_applier.h"

namespace opennova::editor {
class ViewportDevice;
struct EditorRequest;
} // namespace opennova::editor

namespace godot {

// What a Control device hands the Shell (EditorApp, which serves each; the script device's, S13 V10):
// the requests it makes, served at once, from where it raises them (a control's deferred signal,
// outside any pump); the requests it makes as it goes, which it raises inside a pump (the burst's
// EndEdit of a device given up mid-burst), served at the Shell's next pump; and the notices it has
// for the person (an edit it refused), posted on the editor's status line.
struct ViewportDeviceSink {
	std::function<void(const opennova::editor::EditorRequest &)> request;
	std::function<void(const opennova::editor::EditorRequest &)> request_later;
	std::function<void(const std::string &)> notice;
};

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
	std::unique_ptr<opennova::editor::ViewportDevice> (*make_control)(Node &owner, ViewportDeviceSink sink) = nullptr;
};

// A kind's row; null past the last kind.
const ViewportDeviceRow *viewport_device_row(opennova::editor::ViewportKind kind);
// A new device of `kind` (EditorApp's device cache's factory, ViewportDeviceCache), its nodes children
// of `owner`: a SubViewport device's SubViewport handed to `retire` when the device goes (the Shell
// frees it at its next frame), a Control device's requests and notices to `sink`; null past the last
// kind.
std::unique_ptr<opennova::editor::ViewportDevice> make_viewport_device(Node &owner, opennova::editor::ViewportKind kind,
		std::function<void(SubViewport *)> retire, ViewportDeviceSink sink);

} // namespace godot
