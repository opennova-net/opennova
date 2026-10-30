#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <runtime/menu/armory_screen.h>

#include <memory>

#include "mnu/menu_driver.h"

namespace godot {

class ArmoryWorldView;
class PlayerProfiles;
class RtxtStringFile;
class WeaponDatabase;
class WeaponDef;
class WeaponKitEntry;

// The in-game armory's WEAPON screen controller (engine menu::ArmoryScreen, runtime/menu/
// armory_screen.h carries the witnesses) over the running world: its host reads the local player
// and applies the ACCEPT through the ArmoryWorldView's simulation, its weapon table and its
// viewmodel verbs. ArmoryPresenter holds one for the process (the class pages outlive the screen
// and the mission, D-MNU-9) and claims the open weapon.mnu document with it
// (MenuDriver.claim_screen). The device work its controller asks for is a signal: the slot icons
// (icons_changed), the FP viewmodel refresh after a local apply (viewmodel_changed) and the close
// (close_requested).
class ArmoryScreen : public MenuScreenBinding {
	GDCLASS(ArmoryScreen, MenuScreenBinding)

	class Host;
	friend class Host;

	opennova::menu::ArmoryScreen controller_;
	std::unique_ptr<Host> host_;
	Ref<ArmoryWorldView> view_;
	Ref<PlayerProfiles> profiles_;
	Ref<WeaponDatabase> weapons_; // injected; else the view's
	Ref<WeaponDatabase> resolved_; // the table the controller reads now
	Ref<RtxtStringFile> gametext_;
	Ref<RtxtStringFile> menutxt_;
	Ref<RtxtStringFile> gameui_;

	Ref<WeaponDatabase> table_();

protected:
	static void _bind_methods();

public:
	ArmoryScreen();
	~ArmoryScreen() override;

	opennova::menu::MenuScreenController *screen_controller() override { return &controller_; }

	void setup(const Ref<ArmoryWorldView> &p_view);
	void set_player_profiles(const Ref<PlayerProfiles> &p_profiles);
	// A table that stands over the view's (tests, another shell-owned mount); null hands the
	// lists back to the view's.
	void set_weapon_database(const Ref<WeaponDatabase> &p_weapons);
	// The text tables the screen reads: gametext (WepDes), menutxt and Game.bin (the menu's own).
	void set_text_tables(const Ref<RtxtStringFile> &p_gametext, const Ref<RtxtStringFile> &p_menutxt,
			const Ref<RtxtStringFile> &p_gameui);
	// The mission start's stage of the class pages (no simulation: nothing).
	void stage_mission_start();
	// ACCEPT's hotkey accelerator (the use-item keys), past the press latch.
	void trigger_accept() { controller_.accept(); }
	bool is_open() const { return controller_.is_open(); }

	// Read seams (ADR 0018): the class the screen runs against, a category's selected weapon
	// (null NONE) and its AMMO1 row + 1, the slots' icon names, the weight readout, a class page.
	int get_selected_class() const { return controller_.selected_class(); }
	Ref<WeaponDef> selected_weapon(const String &p_control);
	int selected_clips(const String &p_control) const;
	PackedStringArray icon_names() const;
	TypedArray<WeaponKitEntry> get_page(int p_player_class) const;
};

} // namespace godot
