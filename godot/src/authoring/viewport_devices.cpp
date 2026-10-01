#include "authoring/viewport_devices.h"

#include <iterator>

#include "authoring/menu_viewport_applier.h"
#include "authoring/model_viewport_applier.h"
#include "authoring/viewport_device.h"

namespace godot {

namespace {

using opennova::editor::ViewportKind;

std::unique_ptr<ViewportApplier> make_menu_applier(SubViewport &viewport) {
	return std::make_unique<MenuViewportApplier>(viewport);
}
std::unique_ptr<ViewportApplier> make_model_applier(SubViewport &viewport) {
	return std::make_unique<ModelViewportApplier>(viewport);
}

constexpr ViewportDeviceRow kDevices[] = {
	{ ViewportKind::Menu, make_menu_applier },
	{ ViewportKind::Model, make_model_applier },
};

constexpr bool devices_in_order() {
	for (size_t i = 0; i < opennova::editor::kViewportKindCount; ++i)
		if (kDevices[i].kind != static_cast<ViewportKind>(i) || !kDevices[i].make) return false;
	return true;
}

static_assert(std::size(kDevices) == opennova::editor::kViewportKindCount, "every ViewportKind has exactly one device");
static_assert(devices_in_order(), "the viewport devices follow ViewportKind's order, each with its make");

} // namespace

const ViewportDeviceRow *viewport_device_row(ViewportKind kind) {
	const size_t index = static_cast<size_t>(kind);
	return index < opennova::editor::kViewportKindCount ? &kDevices[index] : nullptr;
}

std::unique_ptr<opennova::editor::ViewportDevice> make_viewport_device(Node &owner, ViewportKind kind,
		std::function<void(SubViewport *)> retire) {
	const ViewportDeviceRow *row = viewport_device_row(kind);
	if (!row) return nullptr;
	return std::make_unique<ViewportDevice>(owner, String("Viewport ") + opennova::editor::viewport_kind_token(kind),
			[row](SubViewport &viewport) { return row->make(viewport); }, std::move(retire));
}

} // namespace godot
