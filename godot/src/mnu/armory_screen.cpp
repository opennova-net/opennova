#include "mnu/armory_screen.h"

#include "mnu/mnu_document.h" // MenuDriver's inline document() copies a Ref<MnuDocument>
#include "object/weapon_database.h"
#include "object/weapon_def.h"
#include "player/player_profiles.h"
#include "rtxt/rtxt_string_file.h"
#include "simulation/player_inventory.h"
#include "simulation/simulation.h"
#include "simulation/weapon_kit_entry.h"
#include "util/string_convert.h"
#include "world/world_view.h"

#include <runtime/world/player_loadout.h> // kClassAllowMaskAll

using opennova::to_gd;
using opennova::to_std;

namespace godot {

namespace {

// The typed kit rows as a page (null rows drop) and back.
opennova::playersav::KitPage page_of(const TypedArray<WeaponKitEntry> &p_kit) {
	opennova::playersav::KitPage page;
	for (int i = 0; i < p_kit.size(); ++i) {
		const Ref<WeaponKitEntry> row = p_kit[i];
		if (row.is_null()) continue;
		const opennova::world::WeaponKitEntry &value = row->value();
		page.entries.push_back({ value.name, value.ammo_primary, value.ammo_secondary,
				value.flags });
	}
	return page;
}

TypedArray<WeaponKitEntry> kit_of(const opennova::playersav::KitPage &p_page) {
	TypedArray<WeaponKitEntry> out;
	for (const opennova::playersav::KitEntry &entry : p_page.entries) {
		Ref<WeaponKitEntry> row;
		row.instantiate();
		row->assign({ entry.name, entry.ammo_primary, entry.ammo_secondary, entry.flags });
		out.push_back(row);
	}
	return out;
}

} // namespace

// The controller's host over the world view: the simulation each read resolves anew (each
// mission brings a new one), the weapon table, the text tables and the device signals.
class ArmoryScreen::Host : public opennova::menu::ArmoryScreenHost {
	ArmoryScreen *owner_;

	Ref<Simulation> sim_() const {
		return owner_->view_.is_valid() ? owner_->view_->sim() : Ref<Simulation>();
	}

public:
	explicit Host(ArmoryScreen *p_owner) : owner_(p_owner) {}

	const opennova::def::DefWeaponsFile *weapons() override {
		owner_->resolved_ = owner_->table_();
		return owner_->resolved_.is_valid() ? &owner_->resolved_->native_file() : nullptr;
	}
	opennova::hud::GameTextLookup gametext() override { return game_text_lookup(owner_->gametext_); }
	opennova::hud::GameTextLookup menu_ui() override { return game_text_lookup(owner_->gameui_); }
	opennova::hud::GameTextLookup menutxt() override { return game_text_lookup(owner_->menutxt_); }
	int entity_team() override {
		const Ref<Simulation> sim = sim_();
		return sim.is_valid() ? sim->get_local_player_team() : 1;
	}
	int player_class() override {
		const Ref<Simulation> sim = sim_();
		return sim.is_valid() ? sim->get_local_player_class() : 0;
	}
	uint32_t class_allow_mask() override {
		const Ref<Simulation> sim = sim_();
		return sim.is_valid() ? static_cast<uint32_t>(sim->get_class_allow_mask())
							  : opennova::world::kClassAllowMaskAll;
	}
	// D-MNU-10: the class pair shows offline too (our runtime is a listen session even in SP).
	bool class_selection() override { return true; }
	const opennova::playersav::Record *profile_record() override {
		return owner_->profiles_.is_valid() ? &owner_->profiles_->native().current_weapons()
											: nullptr;
	}
	opennova::playersav::KitPage spawn_page() override {
		const Ref<Simulation> sim = sim_();
		return sim.is_valid() ? page_of(sim->get_local_player_loadout())
							  : opennova::playersav::KitPage();
	}
	opennova::menu::WeaponAvailability availability() override {
		const Ref<Simulation> sim = sim_();
		if (sim.is_null()) return {};
		return [sim](const std::string &p_name) {
			return sim->get_weapon_availability(to_gd(p_name));
		};
	}
	bool press_latched() override {
		const Ref<Simulation> sim = sim_();
		return sim.is_valid() && sim->is_armory_press_latched();
	}
	void reset_all_input_state() override {
		const Ref<Simulation> sim = sim_();
		if (sim.is_valid()) sim->reset_all_input_state();
	}
	bool apply_loadout(const opennova::playersav::KitPage &p_kit, int p_player_class) override {
		const Ref<Simulation> sim = sim_();
		return sim.is_valid() && sim->apply_local_player_loadout(kit_of(p_kit), p_player_class);
	}
	bool in_session() override {
		const Ref<Simulation> sim = sim_();
		return sim.is_valid() && sim->is_mp_session();
	}
	// The apply's select committed and mounted the equipped slot in the sim, its mount request
	// queuing SWITCHRANK on that slot (engine world/player_loadout.cpp select_current_slot): the
	// viewmodel follows it with no second sim mount; a kit that left the table bare equips nothing.
	void present_equipped() override {
		const Ref<Simulation> sim = sim_();
		if (sim.is_null() || owner_->view_.is_null()) return;
		const Ref<PlayerInventory> inventory = sim->get_local_player_inventory();
		const String equipped = inventory.is_valid() ? inventory->get_equipped_name() : String();
		if (equipped.is_empty()) {
			owner_->view_->clear_local_player_weapon();
			owner_->emit_signal("viewmodel_changed");
		} else if (owner_->view_->present_local_player_weapon(equipped)) {
			owner_->emit_signal("viewmodel_changed");
		}
	}
	void close_screen() override { owner_->emit_signal("close_requested"); }
	void icons_changed(const opennova::menu::ArmoryIcons &p_icons) override {
		(void)p_icons;
		owner_->emit_signal("icons_changed");
	}
};

ArmoryScreen::ArmoryScreen() : host_(std::make_unique<Host>(this)) {
	controller_.set_host(host_.get());
}

ArmoryScreen::~ArmoryScreen() { controller_.set_host(nullptr); }

Ref<WeaponDatabase> ArmoryScreen::table_() {
	if (weapons_.is_valid()) return weapons_;
	return view_.is_valid() ? view_->weapon_database() : Ref<WeaponDatabase>();
}

void ArmoryScreen::setup(const Ref<ArmoryWorldView> &p_view) { view_ = p_view; }

void ArmoryScreen::set_player_profiles(const Ref<PlayerProfiles> &p_profiles) {
	profiles_ = p_profiles;
}

void ArmoryScreen::set_weapon_database(const Ref<WeaponDatabase> &p_weapons) {
	weapons_ = p_weapons;
}

void ArmoryScreen::set_text_tables(const Ref<RtxtStringFile> &p_gametext,
		const Ref<RtxtStringFile> &p_menutxt, const Ref<RtxtStringFile> &p_gameui) {
	gametext_ = p_gametext;
	menutxt_ = p_menutxt;
	gameui_ = p_gameui;
}

void ArmoryScreen::stage_mission_start() {
	if (view_.is_null() || view_->sim().is_null()) return;
	controller_.stage_mission_start();
}

Ref<WeaponDef> ArmoryScreen::selected_weapon(const String &p_control) {
	const Ref<WeaponDatabase> table = table_();
	const int32_t row = controller_.selected_weapon(to_std(p_control));
	return table.is_valid() && row >= 0 ? table->get_weapon(row) : Ref<WeaponDef>();
}

int ArmoryScreen::selected_clips(const String &p_control) const {
	return controller_.selected_clips(to_std(p_control));
}

PackedStringArray ArmoryScreen::icon_names() const {
	PackedStringArray out;
	for (const std::string &name : controller_.icons().slot) out.push_back(to_gd(name));
	return out;
}

TypedArray<WeaponKitEntry> ArmoryScreen::get_page(int p_player_class) const {
	return kit_of(controller_.loadout().buffers.page(p_player_class));
}

void ArmoryScreen::_bind_methods() {
	ClassDB::bind_method(D_METHOD("setup", "view"), &ArmoryScreen::setup);
	ClassDB::bind_method(D_METHOD("set_player_profiles", "profiles"),
			&ArmoryScreen::set_player_profiles);
	ClassDB::bind_method(D_METHOD("set_weapon_database", "weapons"),
			&ArmoryScreen::set_weapon_database);
	ClassDB::bind_method(D_METHOD("set_text_tables", "gametext", "menutxt", "gameui"),
			&ArmoryScreen::set_text_tables);
	ClassDB::bind_method(D_METHOD("stage_mission_start"), &ArmoryScreen::stage_mission_start);
	ClassDB::bind_method(D_METHOD("trigger_accept"), &ArmoryScreen::trigger_accept);
	ClassDB::bind_method(D_METHOD("is_open"), &ArmoryScreen::is_open);
	ClassDB::bind_method(D_METHOD("get_selected_class"), &ArmoryScreen::get_selected_class);
	ClassDB::bind_method(D_METHOD("selected_weapon", "control"), &ArmoryScreen::selected_weapon);
	ClassDB::bind_method(D_METHOD("selected_clips", "control"), &ArmoryScreen::selected_clips);
	ClassDB::bind_method(D_METHOD("icon_names"), &ArmoryScreen::icon_names);
	ClassDB::bind_method(D_METHOD("get_page", "player_class"), &ArmoryScreen::get_page);
	// The weight readout's refresh changed the slot icons (icon_names, selected_weapon).
	ADD_SIGNAL(MethodInfo("icons_changed"));
	// An out-of-session ACCEPT presented the equipped slot's viewmodel: the FP view rebuilds.
	ADD_SIGNAL(MethodInfo("viewmodel_changed"));
	// ACCEPT or CANCEL closed the screen (g_WeaponScreenOpen cleared).
	ADD_SIGNAL(MethodInfo("close_requested"));
}

} // namespace godot
