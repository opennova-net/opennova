#include <runtime/inmatch/player_info_screen.h>

#include <base/io/cp1252.h>
#include <base/io/strutil.h>
#include <formats/mnu/mnu.h>
#include <runtime/inmatch/join_character_profile.h>
#include <runtime/menu/menu_runtime.h>
#include <runtime/menu/loadout_labels.h>
#include <runtime/menu/player_info_kit.h>
#include <runtime/profile/player_profiles.h>
#include <runtime/profile/profile_controls.h>
#include <runtime/world/player_loadout.h>

namespace opennova::inmatch {

namespace {

using Change = menu::PlayerInfoAvatars::Change;
using Kind = menu::MenuEvent::Kind;

// The controls the screen's code finds by name [orig: PlayerInfo_RegisterAllControls @0x561470].
constexpr const char *kNationality = "NATIONALITY";
constexpr const char *kDivision = "DIVISION";
constexpr const char *kComboList = "COMBO_LIST";
constexpr const char *kPlayerVoice = "PLAYERVOICE";
constexpr const char *kVoicePreview = "TESTPLAYERVOICE";
constexpr const char *kSideBlue = "SIDE_BLUE";
constexpr const char *kSideRed = "SIDE_RED";
constexpr const char *kPlayerName = "PLAYERNAME";
constexpr const char *kPlayerClass = "PLAYERCLASS";
constexpr const char *kPlayerPreview = "PLAYER_PREVIEW";
constexpr const char *kAutoReload = "OPTIONS_AUTORELOAD";
constexpr const char *kAutoMedic = "OPTIONS_AUTOMEDIC";
constexpr const char *kTotalWeight = "STATIC_TOTAL_WEIGHT";
constexpr const char *kAccept = "ACCEPT";
// The voice labels' second table and section: the menu shell's own Game.bin, "Avatars".
constexpr const char *kAvatarsSection = "Avatars";
constexpr const char *kMenuSection = "Menu";
// Only PRIMARY / SECONDARY author an ammo-TYPE combo (player.mnu's statics FMJ / AP / SP, values
// 0 / 1 / 2); ACCESSORY and the grenades have none.
constexpr const char *kTypeSlots[2] = { "PRIMARY", "SECONDARY" };
constexpr const char *kGrenadeControls[3] = { "GRENADE_AMMO1", "GRENADE_AMMO2", "GRENADE_AMMO3" };

std::string no_text(const char *, const char *, const char *fallback) {
	return std::string(fallback);
}

int category_of(const std::string &control) {
	for (int i = 0; i < 3; ++i)
		if (strutil::iequals(control, menu::kPlayerInfoCategoryControls[i]))
			return i;
	return -1;
}

const avatars::AvatarsFile kNoAvatars{};
const def::DefWeaponsFile kNoWeapons{};

} // namespace

PlayerInfoScreen::PlayerInfoScreen() : gametext_(no_text), menutxt_(no_text), gameui_(no_text) {}

void PlayerInfoScreen::set_avatars(const avatars::AvatarsFile *file) {
	// The registry is made anew each time: a table read again in place keeps its address.
	avatars_ptr_ = file;
	registry_ = file != nullptr ? CharacterRegistry::from_file(*file) : CharacterRegistry();
}

void PlayerInfoScreen::set_weapons(const def::DefWeaponsFile *weapons, bool loaded) {
	const bool changed = weapons != weapons_;
	weapons_ = weapons;
	weapons_loaded_ = weapons != nullptr && loaded;
	if (!changed)
		return;
	// The count picks are keyed by the old table's rows: a table swap drops them rather than
	// misapply them (the game resets every count pair at each populate anyway).
	loadout_.clear_counts();
	if (menu_ != nullptr && !opening_)
		populate_loadout_();
}

void PlayerInfoScreen::set_text(
		hud::GameTextLookup gametext, hud::GameTextLookup menutxt, hud::GameTextLookup gameui) {
	gametext_ = gametext ? std::move(gametext) : hud::GameTextLookup(no_text);
	menutxt_ = menutxt ? std::move(menutxt) : hud::GameTextLookup(no_text);
	gameui_ = gameui ? std::move(gameui) : hud::GameTextLookup(no_text);
}

void PlayerInfoScreen::set_persisted(std::optional<int> team, std::optional<int> player_class,
		const std::optional<PlayerInfoSideSelection> (&sides)[2]) {
	character_.set_persisted(team, player_class, sides);
}

const def::DefWeaponsFile &PlayerInfoScreen::weapons_file_() const {
	return weapons_ != nullptr ? *weapons_ : kNoWeapons;
}

const avatars::AvatarsFile &PlayerInfoScreen::avatars_file_() const {
	return avatars_ptr_ != nullptr ? *avatars_ptr_ : kNoAvatars;
}

// DEFAULT_VOICE / CHARVOICE_%d and NONE are menu UI strings: menutxt's "Menu", then the shell's
// Game.bin "Avatars", else the readable fallback.
std::string PlayerInfoScreen::menu_text_(const char *key, const char *fallback) const {
	const std::string avatars = hud::game_text(gameui_, kAvatarsSection, key, fallback);
	return hud::game_text(menutxt_, kMenuSection, key, avatars.c_str());
}

// A code-page document's runtime holds the page's bytes, which the record takes as they are; a
// Unicode document's holds UTF-8, which the record takes in the code page (D-MNU-49, D-NET-661).
std::string PlayerInfoScreen::record_text_(const std::string &text) const {
	const mnu::Document *doc = menu_ != nullptr ? menu_->document() : nullptr;
	if (doc == nullptr || doc->source_encoding == mnu::SourceEncoding::CodePage)
		return text;
	return utf8_to_retail_text(retail_text_to_utf8(text));
}

// ---- the populate ---------------------------------------------------------------------

void PlayerInfoScreen::open(menu::MenuRuntime &menu, const std::string &, const std::string &) {
	menu_ = &menu;
	opening_ = true;
	if (host_ != nullptr)
		host_->opening();
	character_.set_registry(registry_);
	// The persisted side, else the authored radio's (player.mnu ships SIDE_BLUE checked).
	const int red = menu.widget_id(kSideRed);
	const int authored_team = red >= 0 && menu.is_widget_checked(red) ? 1 : 0;
	team_ = character_.initial_team(authored_team);
	const int blue = menu.widget_id(kSideBlue);
	if (blue >= 0)
		menu.set_widget_checked(blue, team_ == 0);
	if (red >= 0)
		menu.set_widget_checked(red, team_ == 1);
	populate_side_();
	// The loadout: PLAYERCLASS gives the class mask, the side radios the team mask; both filter the
	// slot lists [orig: PlayerInfo_PopulateWeaponSlotLists @ 0x560430].
	restore_player_class_();
	populate_loadout_();
	populate_name_();
	// The auto-reload / auto-medic boxes show the current record's words (profile_controls.h).
	populate_auto_boxes_();
	opening_ = false;
	if (host_ != nullptr)
		host_->opened();
}

void PlayerInfoScreen::close(menu::MenuRuntime &) {
	menu_ = nullptr;
	opening_ = false;
	if (host_ != nullptr)
		host_->closed();
}

// The shown side's lists filled and its saved selection restored, as the screen's build and a
// side switch do. The cascade runs through the first rows on its way to the restored ones; each of
// its legs reads the profile's byte as it stands, and only the last leg's voice, the fill against
// the restored selection, is written (profile::set_voice_override).
void PlayerInfoScreen::populate_side_() {
	restoring_side_ = true;
	restored_voice_ = selected_voice();
	update_avatars_(Change::Team, 0); // cascades into the divisions, the combos and the voices
	restore_character_selection_(team_);
	restoring_side_ = false;
	if (profiles_ != nullptr)
		profile::set_voice_override(profiles_->current(), team_, restored_voice_);
}

// The voice the cascade leaves selected (a pick, or the fill's reset of a value the list does not
// carry) is the shown side's override byte in the current record, written at once for a user's
// pick; a side's restore writes only its last leg's.
void PlayerInfoScreen::update_avatars_(Change change, int value) {
	populating_ = true;
	const int voice = avatars_.update(
			*menu_, avatars_file_(), change, value, team_, selected_voice(), gameui_, menutxt_);
	populating_ = false;
	if (restoring_side_)
		restored_voice_ = voice;
	else if (profiles_ != nullptr)
		profile::set_voice_override(profiles_->current(), team_, voice);
	if (avatars_.preview_changed && host_ != nullptr)
		host_->preview_changed();
}

void PlayerInfoScreen::restore_character_selection_(int side) {
	if (avatars_ptr_ == nullptr || side < 0 || side > 1)
		return;
	const std::optional<PlayerInfoSideSelection> saved = character_.side_selection(side);
	if (!saved)
		return;
	menu::MenuRuntime &menu = *menu_;
	const avatars::AvatarsFile &file = *avatars_ptr_;
	const int nat_index = saved->nationality;
	int visible_row = -1;
	for (size_t row = 0; row < avatars_.nationalities.size(); ++row)
		if (avatars_.nationalities[row] == nat_index) {
			visible_row = static_cast<int>(row);
			break;
		}
	const int nat_combo = menu.widget_id(kNationality);
	if (visible_row < 0 || nat_combo < 0)
		return;
	menu.select_row(nat_combo, visible_row, false);
	update_avatars_(Change::Nationality, visible_row);
	const avatars::AvatarNationality &nationality =
			file.nationalities[static_cast<size_t>(nat_index)];
	const int div_index = saved->division;
	const int div_combo = menu.widget_id(kDivision);
	if (div_index < 0 || static_cast<size_t>(div_index) >= nationality.divisions_count ||
			div_combo < 0)
		return;
	menu.select_row(div_combo, div_index, false);
	update_avatars_(Change::Division, div_index);
	const int combo_index = saved->combo;
	const int combo = menu.widget_id(kComboList);
	if (combo_index < 0 ||
			static_cast<size_t>(combo_index) >=
					nationality.divisions[static_cast<size_t>(div_index)].combos_count ||
			combo < 0)
		return;
	menu.select_row(combo, combo_index, false);
	update_avatars_(Change::Combo, 0);
}

// The current record's class byte of the BLUE side, whichever side is shown (the dialog's save
// writes the class to both) [orig: PlayerInfo_PopulateAllControls @0x5606f0 selects PLAYERCLASS
// from g_CharSelClass with no side term, @0x560782].
void PlayerInfoScreen::restore_player_class_() {
	const int combo = menu_->widget_id(kPlayerClass);
	if (combo < 0 || profiles_ == nullptr)
		return;
	const int player_class =
			profiles_->current_weapons().side(playersav::SideId::Blue).player_class;
	if (player_class < 5 || player_class > 9)
		return;
	const std::string value = std::to_string(player_class);
	for (int row = 0; row < menu_->item_count(combo); ++row)
		if (menu_->item_value(combo, row) == value) {
			menu_->select_row(combo, row, false);
			return;
		}
}

void PlayerInfoScreen::populate_auto_boxes_() {
	if (profiles_ == nullptr)
		return;
	const int reload = menu_->widget_id(kAutoReload);
	if (reload >= 0)
		menu_->set_widget_checked(reload, profile::auto_reload_checked(profiles_->current()));
	const int medic = menu_->widget_id(kAutoMedic);
	if (medic >= 0)
		menu_->set_widget_checked(medic, profile::auto_medic_checked(profiles_->current()));
}

// The PLAYERNAME edit set from the current record's name, as every populate sets it: the edit's
// text set cuts the name's first {hot} marker (MenuRuntime::set_widget_text), then its change
// notice runs the name rule once on the label the set stored, so a kept name renames the record
// and clears its no-name flag, and a refused last character is cut from the edit alone. The label
// is the set's own, never the frame's read-back (D-MNU-49). An unauthored edit sets nothing
// [orig: PlayerInfo_PopulateAllControls @0x5606f0 - the set @0x5608d5].
void PlayerInfoScreen::populate_name_() {
	const int id = menu_->widget_id(kPlayerName);
	if (id < 0 || profiles_ == nullptr)
		return;
	on_name_changed_(menu_->set_widget_text(id, profiles_->current().name));
}

// The PLAYERNAME edit's change notice names the current record as the player types (and once at
// each populate): a label whose last character the edit keeps (or no text) renames it and clears
// its no-name flag; a refused last character is cut and the edit set to the cut, a text set again
// whose notice reaches no handler (profile::PlayerProfiles::name_changed carries the witness)
// [orig: PlayerInfo_HandlePlayerNameChange @0x55fbb0].
void PlayerInfoScreen::on_name_changed_(const std::string &text) {
	if (profiles_ == nullptr)
		return;
	const std::string label = record_text_(text);
	const std::string kept = profiles_->name_changed(label);
	if (kept == label)
		return;
	const int id = menu_->widget_id(kPlayerName);
	if (id >= 0)
		menu_->set_widget_text(id, kept);
}

// ---- the loadout ----------------------------------------------------------------------

// PLAYERCLASS carries values 5..9 (Medic..Engineer); the class mask is the matching power-of-two
// bit (world::player_info_class_mask) [orig: PlayerInfo_SetTeamAndClassMask @ 0x55de60]. With no
// class control every class's weapons show.
int PlayerInfoScreen::selected_class_mask_() const {
	const int combo = menu_->widget_id(kPlayerClass);
	if (combo < 0)
		return world::kClassFilterMaskAll;
	const int row = menu_->selected_row(combo);
	const std::optional<int> value =
			row >= 0 ? strutil::parse_int(menu_->item_value(combo, row)) : std::nullopt;
	return world::player_info_class_mask(value.value_or(0));
}

// PLAYERCLASS's value when it is a class, else the side's remembered class, else the default
// rifleman (join_character_profile.h).
int PlayerInfoScreen::selected_player_class_() const {
	const int combo = menu_->widget_id(kPlayerClass);
	if (combo >= 0) {
		const int row = menu_->selected_row(combo);
		const std::optional<int> value =
				row >= 0 ? strutil::parse_int(menu_->item_value(combo, row)) : std::nullopt;
		if (value && join_player_class_valid(*value))
			return *value;
	}
	return join_player_class_or_default(character_.player_class(team_));
}

// PRIMARY / SECONDARY / ACCESSORY filled for the selected class and team, each led by a NONE row,
// then the shown side's saved kit page restored over them, the ammo lists, the grenades, the
// weight readout and the icons that hang off the selections
// [orig: PlayerInfo_PopulateWeaponSlotLists @ 0x560430 -> PlayerInfo_PopulateWeaponAccessoryAmmoUI
//  @ 0x55e8b0 -> PlayerInfo_UpdateWeightAndWeaponIcons @ 0x55f480].
void PlayerInfoScreen::populate_loadout_() {
	if (weapons_ == nullptr)
		return;
	const int class_mask = selected_class_mask_();
	const int team_mask = world::player_info_team_mask(team_);
	populating_ = true;
	loadout_.fill_slots(
			*menu_, *weapons_, class_mask, team_mask, menu_text_("NONE", "None"), gametext_);
	populating_ = false;
	// The shown side's saved kit page, the one its class byte picks in the current record, into the
	// lists and the picks the ammo fills then read (menu::player_info_restore_kit,
	// PlayerInfoLoadout::restore).
	if (profiles_ != nullptr) {
		const playersav::Side &side = profiles_->current_weapons().side(
				team_ == 0 ? playersav::SideId::Blue : playersav::SideId::Red);
		const playersav::KitPage *page = side.selected_page();
		loadout_.restore(*menu_,
				menu::player_info_restore_kit(
						page != nullptr ? *page : playersav::KitPage{}, *weapons_),
				static_cast<int32_t>(side.player_class), team_);
	}
	for (const char *control : menu::kPlayerInfoCategoryControls)
		populate_slot_ammo_(control);
	populating_ = true;
	loadout_.fill_grenades(*menu_, *weapons_, class_mask, team_mask, gametext_);
	populating_ = false;
	update_weight_();
	if (host_ != nullptr)
		host_->loadout_changed();
}

void PlayerInfoScreen::populate_slot_ammo_(const std::string &control) {
	populating_ = true;
	loadout_.fill_ammo(*menu_, weapons_file_(), control, team_, gametext_);
	populating_ = false;
}

// The weight readout's format, bands and menu tokens are the engine's (loadout_labels.h
// loadout_weight_line).
void PlayerInfoScreen::update_weight_() {
	if (weapons_ == nullptr)
		return;
	const int label = menu_->widget_id(kTotalWeight);
	if (label < 0)
		return;
	menu_->set_widget_text(label,
			menu::loadout_weight_line(loadout_.weight(*menu_, *weapons_), menutxt_, gameui_));
}

// ---- the side, the class and the save ---------------------------------------------------

// A side radio's click, the shown side's as well: the dialog saved into the record, the side it
// leaves with its kit page included, then the screen populated for the clicked side from the
// record.
void PlayerInfoScreen::set_team_(int team) {
	PlayerInfoSnapshot saved;
	save_from_dialog_(saved);
	team_ = team;
	const int blue = menu_->widget_id(kSideBlue);
	const int red = menu_->widget_id(kSideRed);
	if (blue >= 0)
		menu_->set_widget_checked(blue, team == 0);
	if (red >= 0)
		menu_->set_widget_checked(red, team == 1);
	populate_all_controls_();
}

// The screen populated for the shown side from the record, as a side radio and a class pick do
// after their save: the side's character and voice, the class, the boxes and the name, then the
// loadout (the team and class masks re-filter the lists) and its kit page
// [orig: PlayerInfo_SaveAndRepopulate @0x5608fa -> PlayerInfo_PopulateAllControls @0x5606f0].
void PlayerInfoScreen::populate_all_controls_() {
	populate_side_();
	restore_player_class_();
	populate_auto_boxes_();
	populate_name_();
	populate_loadout_();
}

PlayerInfoSnapshot PlayerInfoScreen::snapshot() {
	PlayerInfoSnapshot out;
	if (menu_ == nullptr)
		return out;
	menu::MenuRuntime &menu = *menu_;
	const int combo = menu.widget_id(kComboList);
	out.team = team_;
	out.nationality = avatars_.nationality;
	out.division = avatars_.division;
	out.combo = combo >= 0 ? menu.selected_row(combo) : -1;
	out.player_class = selected_player_class_();
	const int name = menu.widget_id(kPlayerName);
	out.has_name = name >= 0;
	if (out.has_name)
		out.name = record_text_(menu.get_widget_text(name));
	// The class stamped across both sides, the character bytes per side
	// [orig: PlayerInfo_SaveFromDialog @0x55EE3F-0x55EF38]. The voice is not here: its pick already
	// wrote the record.
	const PlayerInfoCharacterState::Snapshot character =
			character_.snapshot(team_, out.nationality, out.division, out.combo, out.player_class);
	out.current = character.current;
	out.sides[0] = character.sides[0];
	out.sides[1] = character.sides[1];
	// No table means there was no loadout choice to commit, kept distinct from a loaded screen
	// whose three selected rows are explicitly NONE, as a missing or empty weapon.def's are
	// (D-MNU-27).
	if (weapons_ != nullptr && weapons_loaded_) {
		out.has_loadout = true;
		for (int i = 0; i < 3; ++i) {
			out.weapon[i] = loadout_.selected_weapon(menu, menu::kPlayerInfoCategoryControls[i]);
			// The recorded picks, -1 = untouched default: the kit tuple's serialized semantic
			// [orig: PlayerInfo_SerializeWeaponLoadout @ 0x55e4b0 writes the saved arrays].
			out.clips[i] = loadout_.selected_clips(menu, menu::kPlayerInfoCategoryControls[i]);
		}
		// The type bytes are the tuple's flags field [orig: g_PlayerInfoAmmoTypePri/Sec].
		for (int i = 0; i < 2; ++i)
			out.ammo_type[i] = loadout_.selected_ammo_type(kTypeSlots[i], team_);
		out.kit = kit_entries();
		// The page the populate restored, which a class pick has already left.
		out.kit_class = loadout_.page_class;
	}
	return out;
}

// The kit page exactly as retail serializes it: consecutive entries of (name, primary ammo count,
// secondary ammo count, ammo-type / flags) - the side's knife, the medic's medpack, the three
// loadout categories, then ALWAYS three grenade slots (menu::player_info_kit_entries carries the
// order and the witnesses) [orig: PlayerInfo_SerializeWeaponLoadout @ 0x55e4b0 -- knife first
// (@0x55e4dc), the class-5 medpack block (@0x55e624), the selections (@0x55e6bb), then the fixed
// g_PlayerInfoGrenadeSlots[0..2] walk (@0x55e7e0)].
std::vector<playersav::KitEntry> PlayerInfoScreen::kit_entries() const {
	if (menu_ == nullptr || weapons_ == nullptr || !weapons_loaded_)
		return {};
	const def::DefWeaponsFile &weapons = *weapons_;
	const menu::WeaponNameLookup name = [&weapons](int32_t row) -> std::string {
		return row >= 0 && static_cast<size_t>(row) < weapons.count
				? std::string(weapons.entries[row].weapon_name)
				: std::string();
	};
	return menu::player_info_kit_entries(
			loadout_.kit_selection(*menu_, team_, selected_player_class_()), name);
}

// The dialog read back into the current record in memory, as the ACCEPT, a side radio and a class
// pick each do first: the two boxes (each when authored, the medic box inverted;
// profile_controls.h), the name when PLAYERNAME is authored, the class to both sides, the
// character, and the kit into the page the last populate restored
// (profile::PlayerProfiles::commit_name and apply_character_selection carry the rules) [orig:
// PlayerInfo_SaveFromDialog @ 0x55ee10].
bool PlayerInfoScreen::save_from_dialog_(PlayerInfoSnapshot &out) {
	if (profiles_ != nullptr) {
		const int reload = menu_->widget_id(kAutoReload);
		if (reload >= 0)
			profile::set_auto_reload(profiles_->current(), menu_->is_widget_checked(reload));
		const int medic = menu_->widget_id(kAutoMedic);
		if (medic >= 0)
			profile::set_auto_medic(profiles_->current(), menu_->is_widget_checked(medic));
	}
	out = snapshot();
	if (profiles_ == nullptr)
		return true;
	if (out.has_name)
		profiles_->commit_name(out.name);
	profile::CharacterSelection selection;
	selection.player_class = out.player_class;
	selection.team = out.team;
	if (out.team >= 0 && out.team <= 1 && out.sides[out.team]) {
		const PlayerInfoSideSelection &side = *out.sides[out.team];
		selection.character = profile::CharacterSelection::Character{ side.avatar_a, side.avatar_b,
			side.avatar_packed };
	}
	if (out.has_loadout) {
		profile::CharacterSelection::Kit kit;
		kit.page_class = out.kit_class;
		kit.entries = out.kit;
		selection.kit = std::move(kit);
	}
	if (profiles_->apply_character_selection(selection))
		return true;
	if (host_ != nullptr)
		host_->save_refused();
	return false;
}

// The ACCEPT: the dialog saved into the current record, then the selection handed to the host for
// the next spawn; the .mnu's own ACTION still navigates [orig: PlayerInfo_SaveFromDialog @
// 0x55ee10].
void PlayerInfoScreen::commit() {
	if (menu_ == nullptr)
		return;
	PlayerInfoSnapshot saved;
	save_from_dialog_(saved);
	if (host_ != nullptr)
		host_->chosen(saved);
}

// ---- the reads --------------------------------------------------------------------------

int PlayerInfoScreen::preview_combo() const {
	if (menu_ == nullptr || avatars_ptr_ == nullptr)
		return -1;
	const avatars::AvatarCombo *combo = avatars_.selected_combo(*menu_, *avatars_ptr_);
	if (combo == nullptr || avatars_.nationality < 0 || avatars_.division < 0)
		return -1;
	const avatars::AvatarDivision &division =
			avatars_ptr_->nationalities[static_cast<size_t>(avatars_.nationality)]
					.divisions[static_cast<size_t>(avatars_.division)];
	return static_cast<int>(combo - division.combos);
}

int PlayerInfoScreen::selected_voice() const {
	return profiles_ != nullptr ? profile::voice_override(profiles_->current(), team_) : 0;
}

int32_t PlayerInfoScreen::selected_weapon(const std::string &control) const {
	return menu_ != nullptr ? loadout_.selected_weapon(*menu_, control) : -1;
}

int32_t PlayerInfoScreen::selected_clips(const std::string &control) const {
	return menu_ != nullptr ? loadout_.selected_clips(*menu_, control) : -1;
}

int32_t PlayerInfoScreen::selected_ammo_type(const std::string &control) const {
	return loadout_.selected_ammo_type(control, team_);
}

// ---- the registrations --------------------------------------------------------------------

void PlayerInfoScreen::on_event(menu::MenuRuntime &menu, const menu::MenuEvent &event) {
	if (menu_ != &menu)
		return;
	switch (event.kind) {
		case Kind::ValueChanged:
			on_value_changed_(event);
			return;
		case Kind::WidgetActivated:
			on_activated_(event.text);
			return;
		case Kind::HoverChanged:
			// Mousing over the preview button drives the zoom and the sway
			// [orig: PlayerInfo_UpdatePlayerPreviewAnimation active test @ 0x55dba0].
			if (event.id >= 0 && event.id == menu.widget_id(kPlayerPreview) && host_ != nullptr)
				host_->preview_hovered(event.flag);
			return;
		case Kind::ScreenChanged:
			if (host_ != nullptr)
				host_->relayout();
			return;
		default:
			return;
	}
}

void PlayerInfoScreen::on_activated_(const std::string &name) {
	// The driver flips the radio's check (with its group) on the click before the activation, so
	// the side handlers only run the team's populate.
	if (strutil::iequals(name, kSideBlue))
		set_team_(0);
	else if (strutil::iequals(name, kSideRed))
		set_team_(1);
	else if (strutil::iequals(name, kVoicePreview)) {
		// The preview prefers the shown side's override; a zero override plays the head's own voice
		// [orig: PlayerInfo_PreviewVoice @0x55ff97].
		const std::string trigger =
				avatars_.voice_preview_trigger(*menu_, avatars_file_(), selected_voice());
		if (!trigger.empty() && host_ != nullptr)
			host_->play_voice(menu::kPlayerInfoVoiceBank, trigger);
	} else if (strutil::iequals(name, kAccept)) {
		commit(); // [orig: PlayerInfo_SaveFromDialog @0x55ee10]
	}
}

// The witnessed per-control recompute graph [orig: PlayerInfo_RegisterAllControls @ 0x561470]:
// the cascade's combos refill below them, a weapon pick refills that slot's ammo, the weight and
// the icons; a count, type or grenade pick records it, then the weight and the icons.
void PlayerInfoScreen::on_value_changed_(const menu::MenuEvent &event) {
	const std::string name = strutil::to_upper(event.text);
	if (event.text2 == "edit" && name == kPlayerName) {
		on_name_changed_(event.text3);
		return;
	}
	if (event.text2 != "combo" || populating_)
		return;
	const int row = event.value;
	if (name == kNationality)
		return update_avatars_(Change::Nationality, row);
	if (name == kDivision)
		return update_avatars_(Change::Division, row);
	if (name == kComboList)
		return update_avatars_(Change::Combo, 0);
	if (name == kPlayerVoice)
		return update_avatars_(Change::Voice, row);
	if (name == kPlayerClass) {
		// A class pick: the dialog saved into the record, the picked class to both sides and the
		// kit into the page of the class it leaves, then the screen populated from the record, the
		// picked class's page restored.
		PlayerInfoSnapshot saved;
		save_from_dialog_(saved);
		populate_all_controls_();
		return;
	}
	const int category = category_of(name);
	if (category >= 0) {
		// [orig: the PRIMARY/SECONDARY/ACCESSORY handlers @ 0x55f710/0x55f790/0x55f810]
		populate_slot_ammo_(name);
	} else {
		// The handlers' picks are PlayerInfoLoadout's (it carries the witnesses): AMMO1 / AMMO2
		// store the row + 1 for the selected weapon, the type list the row's authored value byte
		// for the team, a grenade list its row.
		bool picked = false;
		for (const char *control : menu::kPlayerInfoCategoryControls) {
			const std::string slot(control);
			if (name == slot + "_AMMO1" || name == slot + "_AMMO2") {
				loadout_.pick_clips(*menu_, slot, name == slot + "_AMMO2", row);
				picked = true;
			}
		}
		for (const char *control : kTypeSlots) {
			if (name == std::string(control) + "_AMMO1_TYPE") {
				loadout_.pick_ammo_type(*menu_, control, team_, row);
				picked = true;
			}
		}
		for (int i = 0; i < 3; ++i) {
			if (name == kGrenadeControls[i]) {
				loadout_.pick_grenade(i, row);
				picked = true;
			}
		}
		if (!picked)
			return;
	}
	// The combined weight and icon refresh [orig: @ 0x55f480].
	update_weight_();
	if (host_ != nullptr)
		host_->loadout_changed();
}

} // namespace opennova::inmatch
