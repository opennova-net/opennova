#include <runtime/menu/armory_screen.h>

#include <runtime/menu/loadout_labels.h>
#include <runtime/menu/menu_flow.h>
#include <runtime/menu/menu_runtime.h>
#include <runtime/world/player_loadout.h>

#include <base/io/strutil.h>

#include <algorithm>

namespace opennova::menu {
namespace {

const char *const kCategories[3] = { "PRIMARY", "SECONDARY", "ACCESSORY" };
const char *const kIconWindows[3] = { "PRIMARY_ICON", "SECONDARY_ICON", "ACCESSORY_ICON" };

// The menu's own resource's "Menu" string, "??key??" on a miss
// [orig: TextResource_GetStringWithFallback @0x562ee0 - the "??%s??" sprintf @0x562f16].
std::string menu_ui_text(const hud::GameTextLookup &menu_ui, const char *key) {
	const std::string miss = std::string("??") + key + "??";
	return hud::game_text(menu_ui, "Menu", key, miss.c_str());
}

const playersav::Record &record_or_empty(const playersav::Record *record) {
	static const playersav::Record empty{};
	return record ? *record : empty;
}

} // namespace

void ArmoryScreen::stage_mission_start() {
	if (!host_) return;
	loadout_.buffers.stage(record_or_empty(host_->profile_record()), host_->entity_team(),
			host_->spawn_page());
}

std::string ArmoryScreen::none_label_() {
	// [orig: UI_PopulateThreeCategoryLists @0x566ef4..0x566f05 - g_TextMenuUi "Menu" "NONE"]
	return menu_ui_text(host_->menu_ui(), "NONE");
}

// The INIT's class spin: the five soldier classes, values 5..9, each its Game.bin "Menu"
// CHARCLASS_* string; once, into the authored-empty spin
// [orig: UI_InitWeaponClassSelection @0x567250 - the once flag dword_25E5744 @0x567257, the
//  clear @0x567286, the inserts @0x5672ae..0x567351].
void ArmoryScreen::fill_classes_(MenuRuntime &menu) {
	const int spin = menu.widget_id("PLAYER_CLASS");
	if (spin < 0 || menu.item_count(spin) > 0) return;
	std::vector<std::string> rows;
	const hud::GameTextLookup text = host_->menu_ui();
	for (const world::ArmoryClassCatalogEntry &entry : world::kArmoryClassCatalog)
		rows.push_back(menu_ui_text(text, entry.text_key));
	menu.set_widget_items(spin, rows);
}

void ArmoryScreen::open(MenuRuntime &menu, const std::string &file, const std::string &screen) {
	(void)file;
	menu_ = &menu;
	if (!host_) return;
	populating_ = true;
	fill_classes_(menu);
	// The on-show [orig: UI_InitTeamClassSelection @0x567370]. The class and the team mask
	// [orig: Armory_ResolveSelectedClass @0x567371 -> @0x5642f0]:
	entity_team_ = host_->entity_team();
	const uint32_t class_mask = host_->class_allow_mask() & 0xFFFFu;
	selected_class_ = world::armory_resolve_selected_class(host_->player_class(), class_mask);
	// The spin's rows enabled by the host's mask, then the select by value, row 0 with no row of
	// that value [orig: CSpinListWnd_SetItemEnabled @0x56739e..0x5673f4; SpinList_SelectItemByValue
	// @0x567402 -> @0x64ba50].
	const int spin = menu.widget_id("PLAYER_CLASS");
	if (spin >= 0) {
		enable_class_rows(menu, spin, class_mask);
		int row = 0;
		for (int i = 0; i < world::kArmoryClassCount; ++i)
			if (world::kArmoryClassCatalog[i].class_value == selected_class_) row = i;
		menu.select_row(spin, row, false);
	}
	// The stage under its team gate [orig: @0x56740c..0x567419].
	loadout_.buffers.stage_on_show(record_or_empty(host_->profile_record()), entity_team_,
			host_->spawn_page());
	// The class pair shown in a session, hidden out of one, both found or neither touched
	// [orig: @0x567430..0x567470; OpenNova shows it offline too, D-MNU-10].
	const int pair_label = menu.widget_id("STATIC_PLAYER_CLASS");
	if (spin >= 0 && pair_label >= 0) {
		const bool shown = host_->class_selection();
		menu.set_widget_shown(spin, shown);
		menu.set_widget_shown(pair_label, shown);
	}
	// ACCEPT and CANCEL do the commands the game binds on the screen (menu_commands.h: WEAPON
	// ACCEPT apply_loadout, CANCEL cancel_loadout) [orig: WeaponDef_RegisterUICallbacks - ACCEPT's
	// arg 0 @0x5671f6, CANCEL's arg 1 @0x567214]. ACCEPT's hotkeys, the use-item binding row's two
	// runtime keys on the control "ACCEPT" (@0x7C7650, found @0x567483), are the embedder's key
	// leg: its press reaches accept() [orig: CUIWidget_ResetScreenHotkeys @0x649ce0 called
	// @0x567490; CUIWidget_AddScreenHotkey @0x649e20 called @0x5674a8 / @0x5674c0].
	commands_.wire(menu, true);
	populating_ = false;
	populate_(menu);
	(void)screen;
}

void ArmoryScreen::close(MenuRuntime &menu) {
	(void)menu;
	menu_ = nullptr;
}

// UI_PopulateThreeCategoryLists: the lists, the class's page and the grenade positions, then its
// tail, the weight readout and the icons [orig: @0x566db0, the jump to
// UI_UpdateWeaponWeightDisplay @0x566f56].
void ArmoryScreen::populate_(MenuRuntime &menu) {
	if (const def::DefWeaponsFile *weapons = host_->weapons()) {
		populating_ = true;
		loadout_.populate(menu, *weapons, entity_team_, selected_class_, host_->availability(),
				none_label_(), host_->gametext());
		populating_ = false;
	}
	refresh_weight_(menu);
}

// UI_HandleTeamClassSelection: the page it leaves saved, the populate of the class it picks
// (the spin is live offline too, D-MNU-10) [orig: @0x566f60].
void ArmoryScreen::flip_class_(MenuRuntime &menu, int row) {
	const int pick = std::clamp(row, 0, world::kArmoryClassCount - 1);
	const int from = selected_class_;
	selected_class_ = world::kArmoryClassCatalog[pick].class_value;
	if (const def::DefWeaponsFile *weapons = host_->weapons()) {
		populating_ = true;
		loadout_.flip_class(menu, *weapons, entity_team_, from, selected_class_,
				host_->availability(), none_label_(), host_->gametext());
		populating_ = false;
	}
	refresh_weight_(menu);
}

// UI_UpdateWeaponWeightDisplay: the STATIC_TOTAL_WEIGHT line over the selections and the three
// slot icons [orig: @0x565640; the icons @0x5657a8..0x5658a3].
void ArmoryScreen::refresh_weight_(MenuRuntime &menu) {
	const def::DefWeaponsFile *weapons = host_->weapons();
	if (weapons) {
		const int label = menu.widget_id("STATIC_TOTAL_WEIGHT");
		if (label >= 0)
			menu.set_widget_text(label, loadout_weight_line(loadout_.weight(menu, *weapons),
					host_->menutxt(), host_->menu_ui()));
	}
	ArmoryIcons icons;
	for (int i = 0; i < 3; ++i) {
		if (!weapons || menu.widget_id(kIconWindows[i]) < 0) continue;
		const int32_t row = loadout_.selected_weapon(menu, kCategories[i]);
		if (row >= 0 && static_cast<size_t>(row) < weapons->count)
			icons.slot[i] = weapons->entries[row].loadout_menu_icon;
	}
	icons_ = icons;
	host_->icons_changed(icons_);
}

void ArmoryScreen::on_event(MenuRuntime &menu, const MenuEvent &event) {
	if (!host_ || populating_) return;
	if (event.kind == MenuEvent::Kind::WidgetActivated) {
		switch (commands_.command_of(menu.widget_screen_of(event.id), event.text)) {
		case MenuCommand::ApplyLoadout: accept(); return;
		case MenuCommand::CancelLoadout: cancel(); return;
		default: return;
		}
	}
	if (event.kind != MenuEvent::Kind::ValueChanged) return;
	if (event.text2 == "spinlist" && strutil::iequals(event.text, "PLAYER_CLASS")) {
		flip_class_(menu, event.value);
		return;
	}
	if (event.text2 != "combo") return;
	const def::DefWeaponsFile *weapons = host_->weapons();
	for (const char *category : kCategories) {
		if (!strutil::iequals(event.text, category)) continue;
		// A category's pick: its ammo controls over the new selection, then the readout
		// [orig: UI_OnPrimaryWeaponTypeChanged @0x5662d0 - the readout @0x5665f4; its siblings].
		if (weapons) {
			populating_ = true;
			loadout_.pick_weapon(menu, *weapons, category, host_->gametext());
			populating_ = false;
		}
		refresh_weight_(menu);
		return;
	}
	// The count and type writes, then the readout [orig: UI_OnPrimaryAmmoCountSelected @0x566620
	// - the store @0x56663e, UI_UpdateWeaponWeightDisplay @0x566645; its siblings].
	if (loadout_.pick_count(menu, event.text, event.value)) refresh_weight_(menu);
}

bool ArmoryScreen::apply_from_buffer_(bool skip_apply) {
	// [orig: WeaponLoadout_ApplyFromBuffer @0x565cd0 - !g_WeaponScreenOpenDebounce &&
	//  g_WeaponScreenOpen @0x565cf8]
	if (!host_ || !menu_ || host_->press_latched()) return false;
	host_->reset_all_input_state(); // [orig: Input_ResetAllState @0x565d01]
	if (!skip_apply) {
		// The page the screen shows, serialized into the selected class's buffer, is the kit
		// applied [orig: @0x565d22..0x565d4b].
		static const def::DefWeaponsFile kNoWeapons{};
		const def::DefWeaponsFile *weapons = host_->weapons();
		accepted_ = loadout_.accept(*menu_, weapons ? *weapons : kNoWeapons, entity_team_,
				selected_class_);
		// In a session the apply is the 0x2F alone; out of one it rebuilds the slots, selects
		// and mounts, and the viewmodel follows [orig: the session test @0x565d4d; the
		// select @0x566285 and the mount @0x566297].
		if (host_->apply_loadout(accepted_, selected_class_) && !host_->in_session())
			host_->present_equipped();
	}
	host_->close_screen(); // [orig: g_WeaponScreenOpen = 0 @0x56629f, Game_CloseInGameScreens @0x5662a9]
	return true;
}

void ArmoryScreen::accept() { apply_from_buffer_(false); }

void ArmoryScreen::cancel() { apply_from_buffer_(true); }

int32_t ArmoryScreen::selected_weapon(const std::string &control) const {
	return menu_ ? loadout_.selected_weapon(*menu_, control) : -1;
}

int ArmoryScreen::selected_clips(const std::string &control) const {
	return menu_ ? armory_selected_clips(*menu_, control) : -1;
}

} // namespace opennova::menu
