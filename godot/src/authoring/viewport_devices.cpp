#include "authoring/viewport_devices.h"

#include <iterator>

#include "authoring/effect_viewport_applier.h"
#include "authoring/menu_viewport_applier.h"
#include "authoring/mission_viewport_applier.h"
#include "authoring/model_viewport_applier.h"
#include "authoring/script_device.h"
#include "authoring/texture_viewport_applier.h"
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
std::unique_ptr<opennova::editor::ViewportDevice> make_script_device(Node &owner, ViewportDeviceSink sink) {
	return std::make_unique<ScriptDevice>(owner, std::move(sink));
}
std::unique_ptr<ViewportApplier> make_mission_applier(SubViewport &viewport) {
	return std::make_unique<MissionViewportApplier>(viewport);
}
std::unique_ptr<ViewportApplier> make_texture_applier(SubViewport &viewport) {
	return std::make_unique<TextureViewportApplier>(viewport);
}
std::unique_ptr<ViewportApplier> make_effect_applier(SubViewport &viewport) {
	return std::make_unique<EffectViewportApplier>(viewport);
}

constexpr ViewportDeviceRow kDevices[] = {
	{ ViewportKind::Menu, make_menu_applier, nullptr },
	{ ViewportKind::Model, make_model_applier, nullptr },
	{ ViewportKind::Script, nullptr, make_script_device },
	{ ViewportKind::Mission, make_mission_applier, nullptr },
	{ ViewportKind::Texture, make_texture_applier, nullptr },
	{ ViewportKind::Effect, make_effect_applier, nullptr },
};

constexpr bool devices_in_order() {
	for (size_t i = 0; i < opennova::editor::kViewportKindCount; ++i)
		if (kDevices[i].kind != static_cast<ViewportKind>(i) || (kDevices[i].make != nullptr) == (kDevices[i].make_control != nullptr))
			return false;
	return true;
}

static_assert(std::size(kDevices) == opennova::editor::kViewportKindCount, "every ViewportKind has exactly one device");
static_assert(devices_in_order(),
		"the viewport devices follow ViewportKind's order, each a SubViewport applier's or a Control device's make");

} // namespace

const ViewportDeviceRow *viewport_device_row(ViewportKind kind) {
	const size_t index = static_cast<size_t>(kind);
	return index < opennova::editor::kViewportKindCount ? &kDevices[index] : nullptr;
}

std::unique_ptr<opennova::editor::ViewportDevice> make_viewport_device(Node &owner, ViewportKind kind,
		std::function<void(SubViewport *)> retire, ViewportDeviceSink sink) {
	const ViewportDeviceRow *row = viewport_device_row(kind);
	if (!row) return nullptr;
	if (row->make_control) return row->make_control(owner, std::move(sink));
	return std::make_unique<ViewportDevice>(owner, String("Viewport ") + opennova::editor::viewport_kind_token(kind),
			[row](SubViewport &viewport) { return row->make(viewport); }, std::move(retire));
}

} // namespace godot
