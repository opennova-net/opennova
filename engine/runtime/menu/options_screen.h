#pragma once

#include "menu_runtime.h"
#include <runtime/controls/binding_set.h>

namespace opennova::menu {

// Device events have already crossed the Godot/OS key and button translation.
struct RemapInput {
	enum class Kind { Other, Key, Mouse };
	Kind kind = Kind::Other;
	bool pressed = false, escape = false, ctrl = false, shift = false, repeat = false;
	int vk = 0;
	uint16_t mouse_mask = 0;
};

// Options-owned interaction state. The shell applies the requested model
// persistence / preview effects; it keeps the ConfigFile store (D-CTRL-3).
class OptionsScreen {
public:
	enum Effect {
		None = 0,
		Consumed = 1,
		PersistBindings = 2,
		CommitPreview = 4,
		RestorePreview = 8,
	};
	void prepare(MenuRuntime &menu, const controls::BindingSet &bindings);
	bool is_surface() const { return is_surface_; }
	// The retail Options policy (options_policy.h) on the surface prepare() found, before any
	// settings owner seeds its values: every witnessed slider's range, its value at the minimum;
	// the VIDEO rows pinned to the one renderer path OpenNova ports and locked, GAMMA at its
	// reference and locked, RESOLUTION's last row locked, the preset buttons disabled; the
	// controls not serviced yet locked, showing their forced checks (D-MNU-21). Nothing off the
	// surface.
	void apply_policy(MenuRuntime &menu) const;
	int activate(MenuRuntime &menu, controls::BindingSet &bindings, const std::string &name);
	void arm(MenuRuntime &menu, const controls::BindingSet &bindings, int id, int row);
	int consume(MenuRuntime &menu, controls::BindingSet &bindings, const RemapInput &event);
	void end_remap(MenuRuntime &menu, const controls::BindingSet &bindings, bool refill);
	static void show_ingame_main(MenuRuntime &menu);

private:
	int control_table(const MenuRuntime &menu) const;
	void fill(MenuRuntime &menu, const controls::BindingSet &bindings, int table, int blank = -1);
	void switch_device(MenuRuntime &menu, const controls::BindingSet &bindings,
			controls::Device device);
	bool has_table_ = false, is_surface_ = false;
	controls::Device device_ = controls::Device::Keyboard;
	int table_ = -1, row_ = -1, action_ = -1;
};

} // namespace opennova::menu
