#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>

#include <functional>
#include <memory>

#include <editor/preview/viewport_kinds.h>

#include "authoring/viewport_applier.h"

namespace opennova::editor {
class ViewportDevice;
}

namespace godot {

// A viewport kind's device (ADR 0046 S13 V5): one row per ViewportKind, in its order, in
// authoring/viewport_devices.cpp, which does not build without it (static_asserts, as the kinds'
// and the views' tables): what makes the kind's applier, its runtime nodes under a device's
// SubViewport (the menu's MenuFrame, the model's ObjectModel with its camera and light).
struct ViewportDeviceRow {
	opennova::editor::ViewportKind kind = opennova::editor::ViewportKind::kCount;
	std::unique_ptr<ViewportApplier> (*make)(SubViewport &viewport) = nullptr;
};

// A kind's row; null past the last kind.
const ViewportDeviceRow *viewport_device_row(opennova::editor::ViewportKind kind);
// A new device of `kind`, its SubViewport a child of `owner` (EditorApp's device cache's factory,
// ViewportDeviceCache), handed to `retire` when the device goes (the Shell frees it at its next
// frame); null past the last kind.
std::unique_ptr<opennova::editor::ViewportDevice> make_viewport_device(Node &owner, opennova::editor::ViewportKind kind,
		std::function<void(SubViewport *)> retire);

} // namespace godot
