#include <editor/preview/hud_viewport.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

#include <base/gameprofile/game_type.h>
#include <base/io/strutil.h>
#include <editor/assets/project_asset_source.h>
#include <editor/model/text_document.h>
#include <editor/preview/hud_canvas.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/def/def.h>
#include <runtime/hud/hud_texture_names.h>

namespace opennova::editor {

namespace {

using io::JsonValue;
using io::json_number;
using io::json_string;
using opennova::hud::HudElement;

// The size its device draws at where no canvas sizes it (a headless Shell's): the HUD's design size.
constexpr ViewportState kHeadlessSize{ 1024, 768 };

constexpr const char *kViewTokens[] = { "normal", "binoculars", "night_vision" };

// What each element of the HUD's walk is in a modder's words, and the hudpos.def keys that place or
// colour it, the first the line a Go to opens (the keys as HUD_ParseHudposToken compares them
// [orig: HUD_ParseHudposToken @ 0x59F370]; formats/def/def_hudpos.cpp reads each).
constexpr HudElementWords kWords[] = {
	{ HudElement::SightsCard, "Weapon sights", {} },
	{ HudElement::BreathBar, "Breath bar", { "BREATHTIME" } },
	{ HudElement::ServicePrompt, "Service prompt", {} },
	{ HudElement::InsetCues, "Inset scope cues", {} },
	{ HudElement::GameInfo, "Game info", { "GAMEINFO", "ZONEINFO" } },
	{ HudElement::Frame, "HUD frame", { "STATICFRAME" } },
	{ HudElement::Health, "Health bar", { "HUDHEALTH", "HUDHEALTHBORDER" } },
	{ HudElement::Instruments, "Weapon icon and instruments", { "HUDWPNICON", "HUDGEARTEXT", "CARGOPOS", "HUDAGLTLRX" } },
	{ HudElement::OpticalCues, "Impact distance", { "SHOWIMPACTDISTPOS" } },
	{ HudElement::Stance, "Stance icon", { "HUDSTANCEPOS", "HUDSTANCE", "STANCEICON_COLOR", "ALPHAFADE" } },
	{ HudElement::AmmoCount, "Ammo count", { "AMMOCOUNTPOS", "WEAPON_TEXTCOLOR" } },
	{ HudElement::WeaponName, "Weapon name", { "HUDWEAPONNAME", "WEAPON_TEXTCOLOR" } },
	{ HudElement::ClipIndicator, "Clip and rounds", { "HUDCLIP", "ALPHAFADE" } },
	{ HudElement::Targeting, "Target markers", {} },
	{ HudElement::Crosshair, "Crosshair", {} },
	{ HudElement::Heat, "Heat bar", { "HUDHEAT", "HUDHEATBORDER" } },
	{ HudElement::Clock, "Clock and player count", { "HUDTIMECLOCK", "HUDPLAYERCOUNT" } },
	{ HudElement::Power, "Throw power bar", { "HUDPOWERBAR" } },
	{ HudElement::Waypoint, "Waypoint label", { "HUDWPDINFO" } },
	{ HudElement::TeamIdLine, "Team line", { "HUDTEAMXY" } },
	{ HudElement::WeaponSlotBar, "Weapon slot bar", { "HUDLS_SYSTEM", "HUDLS_SLOT", "HUDLS_BRACKET", "HUDLS_MOREAV" } },
	{ HudElement::ScopeDetails, "Scope readouts", { "HUDSCOPERANGEXY", "HUDSCOPEZEROXY", "HUDSCOPEMAGXY" } },
	{ HudElement::CapturePointLabels, "Capture point labels", {} },
	{ HudElement::LfpPanel, "Zone status panel", { "LFP_FLAGS" } },
	{ HudElement::VehicleBayLogos, "Vehicle bay logos", {} },
	{ HudElement::Spinmap, "Map", { "HUDSPINMAPX1", "HUDSPINMAPY1", "HUDSPINMAPX2", "HUDSPINMAPY2", "MAPCOORDS" } },
	{ HudElement::AttachLabels, "Seat and armory labels", {} },
	{ HudElement::FriendlyTags, "Friendly tags", { "TAGCOLOR_GOOD", "TAGCOLOR_MIDDLE", "TAGCOLOR_BAD" } },
	{ HudElement::VehiclePanel, "Vehicle panel", { "HUDVEHSTANCEPOS", "VEHICLE_HUD" } },
	{ HudElement::Feed, "Messages", { "HUDSYSTEXT", "HUDCHATTEXT", "HUDCHLINE" } },
	{ HudElement::SquadOrders, "Squad orders", { "HUDORDERS" } },
	{ HudElement::Tip, "Tip panel", { "MRCLIPPYNORMAL" } },
	{ HudElement::EndRoundStatistics, "Score panel", {} },
	{ HudElement::MessageLog, "Recent messages", {} },
	{ HudElement::Scoreboard, "Player list", {} },
	{ HudElement::EndRoundOverlay, "End of round", {} },
	{ HudElement::VoiceMenus, "Voice menus", {} },
	{ HudElement::PausedText, "Paused", { "PAUSEDPOS" } },
	{ HudElement::ChatInput, "Chat line", {} },
	{ HudElement::KillAnnouncement, "Kill banner", {} },
	{ HudElement::TipAlternate, "Tip panel (map open)", { "MRCLIPPYALTERNATE" } },
	{ HudElement::Briefing, "Briefing", {} },
	{ HudElement::Objectives, "Objectives", {} },
	{ HudElement::HelpScreen, "Help", {} },
	{ HudElement::QuitDialog, "Quit dialog", {} },
	{ HudElement::NetQuality, "Connection indicators", { "NETWORKINDICATOR" } },
};
static_assert(std::size(kWords) == opennova::hud::kHudElementCount, "every HudElement has its words");

constexpr bool words_in_order() {
	for (size_t i = 0; i < std::size(kWords); ++i)
		if (kWords[i].element != static_cast<HudElement>(i)) return false;
	return true;
}
static_assert(words_in_order(), "the words follow HudElement's order");

// The textures an element draws by a fixed name (runtime/hud/hud_texture_names.h).
std::vector<std::string> fixed_textures(HudElement element) {
	using namespace opennova::hud;
	auto named = [](std::initializer_list<int32_t> slots) {
		std::vector<std::string> out;
		for (int32_t slot : slots)
			if (const char *name = hud_fixed_texture_name(slot)) out.push_back(name);
		return out;
	};
	switch (element) {
	case HudElement::Spinmap:
		return named({ kHudTexMapCompass, kHudTexMapIcons, kHudTexMapWpIndicator, kHudTexMapRadar, kHudTexMapRadarNarrow });
	case HudElement::Tip:
	case HudElement::TipAlternate: return named({ kHudTexTipBox, kHudTexTipKeyboard, kHudTexTipGameplay });
	case HudElement::Scoreboard:
	case HudElement::MessageLog:
	case HudElement::EndRoundStatistics:
	case HudElement::Briefing:
	case HudElement::Objectives:
	case HudElement::HelpScreen: return named({ kHudTexBoxBorder, kHudTexBoxTile });
	case HudElement::NetQuality: return named({ kHudTexNetIcon, kHudTexNetLinkIcon, kHudTexNetNovaWorldIcon });
	case HudElement::LfpPanel:
		return named({ kHudTexLfpTeam1, kHudTexLfpTeam2, kHudTexLfpNeutral, kHudTexLfpTileOwn, kHudTexLfpTileOther });
	case HudElement::Targeting: return named({ kHudTexTarget, kHudTexTargetFriendly });
	case HudElement::VehicleBayLogos: return named({ kHudTexLogoHelo, kHudTexLogoHumm, kHudTexLogoBoat });
	default: break;
	}
	return {};
}

JsonValue options_to_json(const HudViewportOptions &options) {
	JsonValue out = JsonValue::make_object();
	out.set("width", json_number(options.width));
	out.set("height", json_number(options.height));
	out.set("stance", json_number(options.stance));
	out.set("weapon", json_string(options.weapon));
	out.set("clip", json_number(options.clip));
	out.set("reserve", json_number(options.reserve));
	out.set("health", json_number(options.health));
	out.set("view", json_string(hud_preview_view_token(options.view)));
	out.set("damage", json_number(options.damage));
	out.set("detail", json_number(options.detail));
	out.set("crosshair", json_number(options.crosshair));
	out.set("picked", json_string(options.picked));
	out.set("board", JsonValue::make_bool(options.board));
	out.set("game_type", json_string(hud_board_game_type_token(options.game_type)));
	out.set("players", json_number(options.players));
	out.set("sights", JsonValue::make_bool(options.sights));
	out.set("range", json_number(options.range));
	return out;
}

bool whole(const JsonValue &value, int least, int most, int &out) {
	if (!value.is_number() || std::floor(value.number) != value.number || value.number < least || value.number > most)
		return false;
	out = int(value.number);
	return true;
}

// A SetViewport's options: each member optional.
bool read_options(const JsonValue &json, HudViewportOptions &held, std::string &error) {
	if (!json.is_object()) {
		error = "options is an object {width, height, stance, weapon, clip, reserve, health, view, damage, detail, "
		        "crosshair, picked, board, game_type, players, sights, range}.";
		return false;
	}
	HudViewportOptions options = held;
	for (const io::JsonMember &member : json.object) {
		const std::string &key = member.key;
		const JsonValue &value = member.value;
		if (key == "width" || key == "height") {
			if (!whole(value, kHudScreenLeast, kHudScreenMost, key == "width" ? options.width : options.height)) {
				error = "options." + key + " is a whole number of pixels from 240 to 4096.";
				return false;
			}
		} else if (key == "stance") {
			if (!whole(value, 0, 5, options.stance)) {
				error = "options.stance is a HUDSTANCE id, 0 to 5.";
				return false;
			}
		} else if (key == "weapon") {
			if (!value.is_string()) {
				error = "options.weapon is a weapon.def name, \"\" the first it holds, or \"NONE\" for no weapon.";
				return false;
			}
			options.weapon = value.string;
		} else if (key == "clip") {
			if (!whole(value, -1, 9999, options.clip)) {
				error = "options.clip is the rounds in the clip, 0 to 9999, or -1 for a full clip.";
				return false;
			}
		} else if (key == "reserve") {
			if (!whole(value, 0, 9999, options.reserve)) {
				error = "options.reserve is the rounds carried, 0 to 9999.";
				return false;
			}
		} else if (key == "health") {
			if (!whole(value, 0, 100, options.health)) {
				error = "options.health is a percent, 0 to 100.";
				return false;
			}
		} else if (key == "view") {
			if (!value.is_string() || !hud_preview_view_from_token(value.string, options.view)) {
				error = "options.view is normal, binoculars or night_vision.";
				return false;
			}
		} else if (key == "damage") {
			if (!whole(value, 0, kHudDamageMost, options.damage)) {
				error = "options.damage is the damage vignette's alpha, 0 (none) to 192 (its cap).";
				return false;
			}
		} else if (key == "detail") {
			if (!whole(value, 0, 3, options.detail)) {
				error = "options.detail is the HUD detail level, 0 to 3 (3 hides the HUD).";
				return false;
			}
		} else if (key == "crosshair") {
			if (!whole(value, opennova::hud::kHudCrosshairStyleMin, opennova::hud::kHudCrosshairStyleMax,
			            options.crosshair)) {
				error = "options.crosshair is a crosshair style, 0 (cross01.tga) to 24 (cross25.tga).";
				return false;
			}
		} else if (key == "picked") {
			HudElement element = HudElement::kCount;
			if (!value.is_string() || (!value.string.empty() && !opennova::hud::hud_element_from_token(value.string.c_str(), element))) {
				error = "options.picked is an element's token (an item's element), or \"\" for none.";
				return false;
			}
			options.picked = value.string;
		} else if (key == "board" || key == "sights") {
			if (!value.is_bool()) {
				error = "options." + key + " is true or false.";
				return false;
			}
			(key == "board" ? options.board : options.sights) = value.boolean;
		} else if (key == "game_type") {
			if (!value.is_string() || !hud_board_game_type_from_token(value.string, options.game_type)) {
				error = "options.game_type is a session game type: DM, TDM, KOTH, TKOTH, CTF, SD, AD, FB, FM, AAS, CAC or "
				        "COOP.";
				return false;
			}
		} else if (key == "players") {
			if (!whole(value, 0, kHudBoardPlayersMost, options.players)) {
				error = "options.players is the stand-in players the board lists, 0 to 128.";
				return false;
			}
		} else if (key == "range") {
			if (!whole(value, kHudSightsRangeLeast, kHudSightsRangeMost, options.range)) {
				error = "options.range is the metres the sights' aim rests at, 2 to 1000.";
				return false;
			}
		} else {
			error = "Unknown options member \"" + key +
			        "\" (it takes width, height, stance, weapon, clip, reserve, health, view, damage, detail, "
			        "crosshair, picked, board, game_type, players, sights, range).";
			return false;
		}
	}
	held = options;
	return true;
}

// The line (1-based) and column of an offset of `text`, as a text document counts them.
void place_of(const std::string &text, size_t offset, size_t &line, size_t &column) {
	line = 1;
	size_t start = 0;
	for (size_t i = 0; i < offset && i < text.size(); ++i)
		if (text[i] == '\n') {
			++line;
			start = i + 1;
		}
	column = offset - start + 1;
}

bool is_none(const std::string &weapon) {
	return strutil::iequals(weapon, "NONE");
}

} // namespace

const char *hud_view_status_token(HudViewStatus status) {
	switch (status) {
	case HudViewStatus::NoProject: return "no_project";
	case HudViewStatus::NoLayout: return "no_layout";
	case HudViewStatus::Ready: return "ready";
	}
	return "no_layout";
}

const char *hud_preview_view_token(HudPreviewView view) {
	const size_t index = static_cast<size_t>(view);
	return index < std::size(kViewTokens) ? kViewTokens[index] : "normal";
}

bool hud_preview_view_from_token(const std::string &token, HudPreviewView &out) {
	for (size_t i = 0; i < std::size(kViewTokens); ++i)
		if (token == kViewTokens[i]) {
			out = static_cast<HudPreviewView>(i);
			return true;
		}
	return false;
}

bool HudViewportOptions::operator==(const HudViewportOptions &other) const {
	return width == other.width && height == other.height && stance == other.stance && weapon == other.weapon &&
	       clip == other.clip && reserve == other.reserve && health == other.health && view == other.view &&
	       damage == other.damage && detail == other.detail && crosshair == other.crosshair && picked == other.picked &&
	       board == other.board && game_type == other.game_type && players == other.players && sights == other.sights &&
	       range == other.range;
}

const std::vector<uint32_t> &hud_board_game_types() {
	using namespace opennova::game_type;
	static const std::vector<uint32_t> types = { kDeathmatch, kTeamDeathmatch, kKingOfTheHill, kTeamKingOfTheHill,
		kCaptureTheFlag, kSearchAndDestroy, kAttackDefend, kFlagBall, kFlagMe, kAdvanceAndSecure, kConquerAndControl,
		kObjectiveCoop };
	return types;
}

const char *hud_board_game_type_token(uint32_t game_type) {
	return opennova::game_type::host_abbreviation_key(game_type);
}

bool hud_board_game_type_from_token(const std::string &token, uint32_t &out) {
	for (const uint32_t type : hud_board_game_types())
		if (strutil::iequals(token, hud_board_game_type_token(type))) {
			out = type;
			return true;
		}
	return false;
}

opennova::hud::HudScoreboardState hud_preview_board(const HudViewportOptions &options,
		const opennova::hud::GameTextLookup &gametext, bool gametext_loaded, const opennova::hud::GameTextLookup &keyhelp) {
	opennova::hud::HudScoreboardState board;
	board.game_type = options.game_type;
	board.team_count = opennova::game_type::active_team_count(options.game_type, 2);
	board.local_team = 1;
	for (int i = 0; i < options.players; ++i) {
		opennova::hud::ScoreboardEntry row;
		row.slot_id = uint8_t(i + 1);
		row.score1 = int16_t(options.players - i);
		row.team = board.team_count > 0 ? uint8_t(i % board.team_count + 1) : uint8_t(0);
		row.has_entity = true;
		row.name = "Player " + std::to_string(i + 1);
		row.quality = uint8_t(i % 3 + 1);
		row.player_class = uint8_t(5 + i % 5);
		board.rows.push_back(std::move(row));
	}
	const opennova::hud::ScoreboardHeaderStrings strings = opennova::hud::scoreboard_header_strings(
			gametext, gametext_loaded, keyhelp, board.game_type, options.players, 0);
	board.title = strings.title;
	board.game_type_label = strings.game_type_label;
	board.players_line = strings.players_line;
	board.spectators_line = strings.spectators_line;
	board.footer = strings.footer;
	return board;
}

io::JsonValue hud_options_json(const HudViewportOptions &options) {
	return options_to_json(options);
}

std::string hud_options_change(const HudViewportOptions &options) {
	return viewport_change(ViewportKind::Hud, "options", options_to_json(options));
}

const HudElementWords &hud_element_words(HudElement element) {
	const size_t index = static_cast<size_t>(element);
	return kWords[index < std::size(kWords) ? index : 0];
}

HudViewport::HudViewport(std::string path) : ViewportModel(ViewportKind::Hud, std::move(path), kHeadlessSize) {}

std::unique_ptr<ViewportModel> HudViewport::make(const std::string &path) {
	return std::make_unique<HudViewport>(path);
}

const HudPreviewWeapon *HudViewport::weapon_shown() const {
	if (weapons_.empty() || is_none(options_.weapon)) return nullptr;
	if (options_.weapon.empty()) return &weapons_.front();
	for (const HudPreviewWeapon &weapon : weapons_)
		if (strutil::iequals(weapon.name, options_.weapon)) return &weapon;
	return nullptr;
}

const world::LocalPlayerViewFrame *HudViewport::scope_frame() const {
	return scope_valid_ && range_ ? &range_->view_frame() : nullptr;
}

const world::LocalPlayerWeaponView *HudViewport::scope_weapon() const {
	return scope_valid_ && range_ ? &range_->weapon_view() : nullptr;
}

void HudViewport::follow_board_(const FileSource &files) {
	// A table as its reader reads it, read again when its stamp moves.
	const auto read = [&](const char *name, uint64_t &stamp, bool &loaded, rtxt::File &table) {
		const uint64_t now = files.stamp(name);
		if (tables_read_ && now == stamp) return;
		stamp = now;
		std::vector<uint8_t> bytes;
		std::string error;
		table = rtxt::File();
		loaded = files.read(name, bytes) && !bytes.empty() && rtxt::parse(bytes.data(), bytes.size(), table, error);
	};
	read("gametext.bin", gametext_stamp_, gametext_loaded_, gametext_);
	read("keyhelp.bin", keyhelp_stamp_, keyhelp_loaded_, keyhelp_);
	tables_read_ = true;
	const auto lookup = [](const rtxt::File &table, bool loaded) -> opennova::hud::GameTextLookup {
		return [&table, loaded](const char *section, const char *key, const char *fallback) {
			const rtxt::Entry *entry = loaded ? table.find_in_section(section, key) : nullptr;
			return entry ? entry->text : std::string(fallback);
		};
	};
	board_ = hud_preview_board(options_, lookup(gametext_, gametext_loaded_), gametext_loaded_,
			lookup(keyhelp_, keyhelp_loaded_));
}

void HudViewport::follow_scope_(const SessionView &view) {
	scope_valid_ = false;
	scope_why_.clear();
	if (!options_.sights) return;
	const HudPreviewWeapon *weapon = weapon_shown();
	if (!weapon) {
		scope_why_ = "No weapon is held: the sights are a weapon's.";
		return;
	}
	if (options_.view == HudPreviewView::Binoculars) {
		// The game's frame takes the binoculars first: no card, no readouts while they are up.
		scope_why_ = "The binoculars are up: the game's frame draws them in the sights' place.";
		return;
	}
	if (options_.view == HudPreviewView::NightVision) {
		scope_why_ = "Under night vision the sights draw into the goggles' image, which the preview has none of.";
		return;
	}
	if (!range_) range_ = std::make_unique<WeaponRange>();
	WeaponRangeSetup setup;
	setup.files = view.findings.assets;
	setup.catalog = "weapon.def";
	setup.weapon = weapon->name;
	setup.target.range = float(options_.range);
	range_->configure(setup);
	if (!range_->ready()) {
		scope_why_ = range_->why();
		return;
	}
	// The scope toggled at the run's start, then the run until it settles and, a cycle of the body's aim ray
	// later, the ray has found the wall (the aim's range is acquired every 16 ticks [orig:
	// Entity_UpdateInfantryPlayerBody @0x4B4E9B..0x4B5215]).
	range_->set_gestures({ WeaponGestureAt{ 0, WeaponGesture::Scope } });
	constexpr int32_t kAimTicks = 16;
	constexpr int32_t kMostTicks = 62 * 5;
	if (range_->tick() == 0) {
		int32_t settled = -1;
		for (int32_t tick = 1; tick <= kMostTicks; ++tick) {
			range_->run_to(tick);
			if (settled < 0 && range_->scoped()) settled = tick;
			if (settled >= 0 && tick >= settled + kAimTicks) break;
		}
	}
	if (!range_->scoped()) {
		scope_why_ = "The scope does not come up for " + weapon->name + ".";
		for (const WeaponRangeEvent &event : range_->events())
			if (event.kind == WeaponRangeEvent::Kind::Refused) scope_why_ = event.words;
		return;
	}
	scope_valid_ = true;
}

const HudPreviewElement *HudViewport::element_at(float x, float y) const {
	const HudPreviewElement *found = nullptr;
	float area = 0.0f;
	for (const HudPreviewElement &element : elements_) {
		if (x < element.x0 || y < element.y0 || x > element.x1 || y > element.y1) continue;
		const float a = (element.x1 - element.x0) * (element.y1 - element.y0);
		if (!found || a < area) {
			found = &element;
			area = a;
		}
	}
	return found;
}

const HudPreviewElement *HudViewport::picked() const {
	HudElement element = HudElement::kCount;
	if (options_.picked.empty() || !opennova::hud::hud_element_from_token(options_.picked.c_str(), element)) return nullptr;
	for (const HudPreviewElement &shown : elements_)
		if (shown.element == element) return &shown;
	return nullptr;
}

std::string HudViewport::element_words(const HudPreviewElement &element) const {
	std::string out = hud_element_words(element.element).words;
	if (element.lines.empty()) {
		const bool placed = hud_element_words(element.element).keys[0] != nullptr;
		out += placed ? ": no line of hudpos.def places it" : ": the game places it";
	}
	for (size_t i = 0; i < element.lines.size(); ++i)
		out += (i == 0 ? ": " : ", ") + element.lines[i].key + " (line " + std::to_string(element.lines[i].line) + ")";
	for (size_t i = 0; i < element.textures.size(); ++i)
		out += (i == 0 ? "; draws " : ", ") + element.textures[i].name +
		       (element.textures[i].path.empty() ? std::string(" (the project lacks it)") : std::string());
	return out;
}

ViewportStatus HudViewport::status() const {
	return reason_ == HudViewStatus::Ready ? ViewportStatus::Ready : ViewportStatus::Empty;
}

std::string HudViewport::message() const {
	switch (reason_) {
	case HudViewStatus::NoProject: return "No project is open.";
	case HudViewStatus::NoLayout: return "Open hudpos.def to see the HUD it lays out.";
	case HudViewStatus::Ready: break;
	}
	return std::string();
}

std::string HudViewport::caption() const {
	if (reason_ != HudViewStatus::Ready) return std::string();
	return " - " + std::to_string(options_.width) + " x " + std::to_string(options_.height);
}

ViewportAction HudViewport::stop_(HudViewStatus reason) {
	reason_ = reason;
	detail_.clear();
	boxes_.clear();
	elements_.clear();
	shown_none();
	return picture_.stop();
}

void HudViewport::read_layout_(const TextDocument &text) {
	text_ = text.text();
	lines_ = def::hud_layout_lines(text_);
	// The names the layout hands the HUD's loader, through the engine's own fill over the game's parse
	// of the text as Save writes it (each line CR LF).
	auto model = std::make_shared<HudLayoutModel>();
	assets_ = opennova::hud::HudLayoutAssets();
	stance_names_ = {};
	model_.reset();
	if (model->read(text_)) {
		const def::DefHudPosFile &file = model->file;
		opennova::hud::HudLayout layout;
		opennova::hud::hud_layout_from_hudpos(file, layout, assets_);
		for (size_t i = 0; i < file.hud.stances_count; ++i) {
			const def::DefHudStance &stance = file.hud.stances[i];
			if (stance.id >= 0 && stance.id < 6) stance_names_[size_t(stance.id)] = stance.name;
		}
		model_ = std::move(model);
	}
}

HudLayoutModel::~HudLayoutModel() {
	def::def_free_hudpos(&file);
}

bool HudLayoutModel::read(const std::string &text) {
	def::def_free_hudpos(&file);
	std::string written;
	written.reserve(text.size() + text.size() / 16);
	for (size_t i = 0; i < text.size(); ++i) {
		if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) written.push_back('\r');
		written.push_back(text[i]);
	}
	return def::def_parse_hudpos_memory(reinterpret_cast<const uint8_t *>(written.data()), written.size(), &file) == 0;
}

void HudViewport::read_weapons_(const FileSource &files) {
	const uint64_t stamp = files.stamp("weapon.def");
	if (weapons_read_ && stamp == weapons_stamp_) return;
	weapons_read_ = true;
	weapons_stamp_ = stamp;
	weapons_.clear();
	std::vector<uint8_t> bytes;
	if (!files.read("weapon.def", bytes)) return;
	def::DefWeaponsFile file{};
	if (def::def_parse_weapons_memory(bytes.data(), bytes.size(), &file) == 0) {
		for (size_t i = 0; i < file.count; ++i) {
			const def::DefWeaponDef &def = file.entries[i];
			HudPreviewWeapon weapon;
			weapon.name = def.weapon_name;
			weapon.clipsize = def.clipsize;
			weapon.hudicon = def.hudicon;
			weapon.clip_art = def.hudclipgfx_texture;
			weapon.round_art = def.hudrndgfx_texture;
			if (!weapon.name.empty()) weapons_.push_back(std::move(weapon));
		}
	}
	def::def_free_weapons(&file);
}

void HudViewport::make_elements_() {
	elements_.clear();
	const HudPreviewWeapon *weapon = weapon_shown();
	for (size_t index = 0; index < boxes_.size() && index < opennova::hud::kHudElementCount; ++index) {
		const ViewportDeviceReport::Rect &box = boxes_[index];
		if (!box.placed) continue;
		HudPreviewElement element;
		element.element = static_cast<HudElement>(index);
		element.x0 = float(box.left);
		element.y0 = float(box.top);
		element.x1 = float(box.right);
		element.y1 = float(box.bottom);
		HudDragStart start;
		element.movable = model_ && hud_drag_start(element.element, model_->file.hud, start);
		element.resizable = element.movable && hud::hud_element_resizable(element.element);
		// The lines that place it: each key's the game takes (a HUDSTANCE's of the stance shown).
		const HudElementWords &words = hud_element_words(element.element);
		for (const char *key : words.keys) {
			if (!key) break;
			const std::string stance = std::to_string(options_.stance);
			const bool by_stance = strutil::iequals(key, "HUDSTANCE");
			const def::HudLayoutLine *line = def::hud_layout_line(lines_, key, by_stance ? stance.c_str() : nullptr);
			if (!line) continue;
			HudPreviewElement::Line at;
			at.key = key;
			place_of(text_, line->offset, at.line, at.column);
			at.text = text_.substr(line->offset, line->length);
			element.lines.push_back(std::move(at));
		}
		// The textures it draws: the layout's names, the weapon's art, the fixed names.
		std::vector<std::string> names;
		switch (element.element) {
		case HudElement::Frame: names.push_back(assets_.static_frame); break;
		case HudElement::Stance: names.push_back(assets_.stance_textures[size_t(std::clamp(options_.stance, 0, 5))]); break;
		case HudElement::Instruments:
			if (weapon) names.push_back(weapon->hudicon);
			break;
		case HudElement::ClipIndicator:
			if (weapon) {
				names.push_back(weapon->clip_art);
				names.push_back(weapon->round_art);
			}
			break;
		case HudElement::Crosshair: names.push_back(opennova::hud::hud_crosshair_texture_name(options_.crosshair)); break;
		case HudElement::WeaponSlotBar:
			names.push_back(assets_.hudls_bracket);
			names.push_back(assets_.hudls_moreav);
			break;
		default: names = fixed_textures(element.element); break;
		}
		for (const std::string &name : names) {
			if (name.empty()) continue;
			HudPreviewElement::Art art;
			art.name = name;
			if (assets_source_) art.path = assets_source_->path_of(name);
			element.textures.push_back(std::move(art));
		}
		elements_.push_back(std::move(element));
	}
}

ViewportAction HudViewport::follow_(const ViewportInput &input, PreviewClock &) {
	const SessionView &view = input.view;
	if (!view.project.open || !view.findings.assets) return stop_(HudViewStatus::NoProject);
	const TextDocument *text = input.document ? text_of(*input.document) : nullptr;
	if (!text || input.document->kind() != AssetKind::HudPosDefs) return stop_(HudViewStatus::NoLayout);
	const FileSource &files = *view.findings.assets;
	const uint64_t generation = view.findings.assets->generation();
	assets_source_ = view.findings.assets;
	read_weapons_(files);
	follow_scope_(view);
	follow_board_(files);
	// The text read again only when it changed.
	if (input.document->identity() != read_identity_ || input.document->load_generation() != read_load_ ||
	    input.document->revision() != read_revision_) {
		read_identity_ = input.document->identity();
		read_load_ = input.document->load_generation();
		read_revision_ = input.document->revision();
		read_layout_(*text);
	}
	reason_ = HudViewStatus::Ready;
	detail_.clear();
	make_elements_();
	const bool moved = input.change != ChangeClass::None;
	if (picture_.follow(PreviewFollow::Key{}, moved, files, generation) == PreviewFollow::Found::Same) {
		shown(*input.document);
		return ViewportAction::Keep;
	}
	picture_.show(PreviewFollow::Key{}, generation);
	shown(*input.document);
	// The device makes its picture anew over the layout as Save would write it; what it reads it reports.
	return picture_.built(FileStamps());
}

bool HudViewport::takes_(const std::string &member) const {
	return member == "options";
}

bool HudViewport::check_(const io::JsonValue &json, std::string &error) const {
	HudViewportOptions options = options_;
	const JsonValue *member = json.get("options");
	return !member || read_options(*member, options, error);
}

void HudViewport::apply_(const io::JsonValue &json, PreviewClock &) {
	std::string error;
	if (const JsonValue *member = json.get("options")) read_options(*member, options_, error);
	make_elements_();
}

bool HudViewport::report_(const ViewportDeviceReport &report) {
	picture_.read(report.files);
	const bool moved = report.rects.size() != boxes_.size() ||
	                   !std::equal(report.rects.begin(), report.rects.end(), boxes_.begin(),
	                               [](const ViewportDeviceReport::Rect &a, const ViewportDeviceReport::Rect &b) {
		                               return a.placed == b.placed && a.left == b.left && a.top == b.top &&
		                                      a.right == b.right && a.bottom == b.bottom;
	                               });
	if (moved) {
		boxes_ = report.rects;
		make_elements_();
	}
	return false;
}

std::unique_ptr<CanvasHalf> HudViewport::make_canvas() const {
	return std::make_unique<HudCanvas>();
}

ViewportHit HudViewport::hit(const ViewportContext &context, float x, float y) const {
	ViewportHit out;
	out.current = current(context.input);
	const HudPreviewElement *element = element_at(x, y);
	if (!element) return out;
	out.index = int(element->element);
	out.name = element_words(*element);
	out.kind = opennova::hud::hud_element_token(element->element);
	return out;
}

bool HudViewport::handle_point(const ViewportContext &, NodeId, const std::string &, float &, float &,
		std::string &error) const {
	error = "A HUD element is no record: its handles are the canvas's, and a command moves or resizes it "
	        "(move, resize, by its item).";
	return false;
}

bool HudViewport::drag(const ViewportContext &, const ViewportDrag &, CanvasRequests &, std::string &error) const {
	error = "A HUD element is no record to drag by its id: the commands move and resize take it by its item "
	        "(its element's token).";
	return false;
}

bool HudViewport::command(const ViewportContext &, const std::string &name, const std::vector<NodeId> &,
		CanvasRequests &, std::string &error) const {
	error = "The HUD's picture has no command \"" + name + "\" (it takes move, resize, set and click; its state is "
	        "set with set_viewport's options).";
	return false;
}

bool HudViewport::element_named(const std::string &item, HudElement &out, std::string &error) const {
	const std::string &token = item.empty() ? options_.picked : item;
	if (token.empty()) {
		error = "No element is picked: name one by its item (its element's token, \"ammo_count\").";
		return false;
	}
	if (!opennova::hud::hud_element_from_token(token.c_str(), out)) {
		error = "\"" + token + "\" is no HUD element's token.";
		return false;
	}
	return true;
}

bool HudViewport::read_now(const ViewportContext &context, HudLayoutModel &out, const TextDocument *&text,
		std::string &error) const {
	text = context.input.document ? text_of(*context.input.document) : nullptr;
	if (!text || context.input.document->kind() != AssetKind::HudPosDefs) {
		error = "No HUD layout is open at " + path() + ".";
		return false;
	}
	if (!out.read(text->text())) {
		error = "The HUD layout's text does not read.";
		return false;
	}
	return true;
}

bool HudViewport::plan_changes(const ViewportContext &context, const std::vector<HudValueChange> &changes,
		uint64_t gesture, CanvasRequests &out, std::string &error) const {
	// The text as it is now, read as the game reads it (its lines as Save writes them).
	HudLayoutModel now;
	const TextDocument *text = nullptr;
	if (!read_now(context, now, text, error)) return false;
	if (!context.editable()) {
		error = context.not_editable();
		return false;
	}
	std::vector<Edit> edits;
	if (!hud_layout_edits(*text, now.file.hud, changes, gesture, edits, error)) return false;
	if (!edits.empty()) out.request(request::edit_record(path(), std::move(edits)));
	return true;
}

bool HudViewport::command_of(const ViewportContext &context, const ViewportCommand &command, CanvasRequests &out,
		std::string &error) const {
	const std::string &name = command.name;
	if (name != "move" && name != "resize" && name != "set") {
		if (!command.item.empty() || !command.handle.empty() || !command.field.empty() || !command.value.empty()) {
			error = "The HUD's command \"" + name + "\" takes no item, handle, field or value.";
			return false;
		}
		return ViewportModel::command_of(context, command, out, error);
	}
	if (!command.ids.empty() || command.mode != SelectMode::Replace) {
		error = "A HUD element's " + name + " takes its item, never record ids or a mode.";
		return false;
	}
	if (reason_ != HudViewStatus::Ready || !current(context.input)) {
		error = "The viewport shows no picture of the HUD layout as it is now.";
		return false;
	}
	HudElement element = HudElement::kCount;
	if (!element_named(command.item, element, error)) return false;
	HudLayoutModel now;
	const TextDocument *text = nullptr;
	if (!read_now(context, now, text, error)) return false;
	const def::DefHudPosDef *model = &now.file.hud;
	std::vector<HudValueChange> changes;
	if (name == "set") {
		if (command.field.empty() || !command.by.empty() || command.has_at || !command.handle.empty()) {
			error = "set takes a field (one of the element's fields by its id) and its value, nothing else.";
			return false;
		}
		HudValueChange change;
		if (!hud_field_change(element, *model, command.field, command.value, change, error)) return false;
		changes.push_back(std::move(change));
		return plan_changes(context, changes, 0, out, error);
	}
	if (!command.field.empty() || !command.value.empty()) {
		error = name + " takes no field or value (set does).";
		return false;
	}
	HudDragStart start;
	if (!hud_drag_start(element, *model, start)) {
		error = std::string("No line of hudpos.def places the ") + opennova::hud::hud_element_token(element) +
		        ": the game places it.";
		return false;
	}
	HudHandle handle = HudHandle::Move;
	float dx = 0.0f, dy = 0.0f;
	if (name == "move") {
		if (!command.handle.empty() && command.handle != "move") {
			error = "move takes no handle (resize does).";
			return false;
		}
		if (command.has_at == (command.by.size() == 2)) {
			error = "move goes by [dx, dy] design units or at [x, y], its place there, one of them.";
			return false;
		}
		if (command.has_at) {
			// Its lead place on each axis (its first point or near edge) to the point.
			bool led[2] = { false, false };
			for (size_t i = 0; i < start.coordinates.size(); ++i) {
				const hud::HudCoordinate &coordinate = start.coordinates[i];
				const int axis = coordinate.axis == hud::HudAxis::X ? 0 : 1;
				if (led[axis] || coordinate.edge == hud::HudEdge::Extent) continue;
				led[axis] = true;
				(axis == 0 ? dx : dy) = (axis == 0 ? command.at_x : command.at_y) - float(start.values[i]);
			}
		} else {
			dx = float(command.by[0]);
			dy = float(command.by[1]);
		}
	} else {
		if (command.has_at || command.by.size() != 2) {
			error = "resize goes by [dx, dy] design units.";
			return false;
		}
		handle = HudHandle::BottomRight;
		if (!command.handle.empty() && (!hud_handle_from_token(command.handle, handle) || handle == HudHandle::Move)) {
			error = "resize takes a corner's handle: top_left, top_right, bottom_left or bottom_right.";
			return false;
		}
		dx = float(command.by[0]);
		dy = float(command.by[1]);
	}
	if (!hud_drag_changes(start, handle, dx, dy, 1, changes, error)) return false;
	return plan_changes(context, changes, 0, out, error);
}

bool HudViewport::click_frame(const ViewportContext &context, SelectMode mode, int &width, int &height,
		std::string &error) const {
	if (reason_ != HudViewStatus::Ready || !current(context.input)) {
		const std::string why = message();
		error = "The viewport shows no picture of the HUD layout as it is now" + (why.empty() ? std::string(".") : ": " + why);
		return false;
	}
	if (mode != SelectMode::Replace) {
		error = "A click picks one element of the HUD: it takes no Shift or Ctrl.";
		return false;
	}
	// The click's point in the screen's pixels: the canvas reads the picture at the screen's size.
	width = options_.width;
	height = options_.height;
	return true;
}

io::JsonValue HudViewport::options_json() const {
	return options_to_json(options_);
}

io::JsonValue HudViewport::body_json(const ViewportInput &) const {
	JsonValue out = JsonValue::make_object();
	JsonValue screen = JsonValue::make_object();
	screen.set("width", json_number(options_.width));
	screen.set("height", json_number(options_.height));
	out.set("screen", std::move(screen));
	// The HUD font the HUD slot loads at this width (the HI name above 640, the LO name at 640 and below).
	out.set("hud_font", json_string(opennova::hud::hudpos_font_for_width(assets_, options_.width)));
	out.set("static_frame", json_string(assets_.static_frame));
	JsonValue stances = JsonValue::make_array();
	for (size_t i = 0; i < stance_names_.size(); ++i) {
		JsonValue stance = JsonValue::make_object();
		stance.set("id", json_number(double(i)));
		stance.set("name", json_string(stance_names_[i]));
		stance.set("texture", json_string(assets_.stance_textures[i]));
		stances.push(std::move(stance));
	}
	out.set("stances", std::move(stances));
	const HudPreviewWeapon *weapon = weapon_shown();
	out.set("weapon_shown", json_string(weapon ? weapon->name : std::string()));
	JsonValue weapons = JsonValue::make_array();
	for (const HudPreviewWeapon &each : weapons_) {
		JsonValue row = JsonValue::make_object();
		row.set("name", json_string(each.name));
		row.set("clipsize", json_number(each.clipsize));
		row.set("hudicon", json_string(each.hudicon));
		row.set("clip_art", json_string(each.clip_art));
		row.set("round_art", json_string(each.round_art));
		weapons.push(std::move(row));
	}
	out.set("weapons", std::move(weapons));
	out.set("elements", json_number(double(elements_.size())));
	// The Tab board, and the sights' frame: whether the card and the readouts draw, the selectors, what the
	// readouts read (the aim's range in metres, the zero word, the magnification), and why not.
	JsonValue board = JsonValue::make_object();
	board.set("shown", JsonValue::make_bool(options_.board));
	board.set("game_type", json_string(hud_board_game_type_token(options_.game_type)));
	board.set("rows", json_number(options_.board ? options_.players : 0));
	board.set("teams", json_number(opennova::game_type::active_team_count(options_.game_type, 2)));
	out.set("board", std::move(board));
	JsonValue sights = JsonValue::make_object();
	const world::LocalPlayerViewFrame *frame = scope_frame();
	sights.set("up", JsonValue::make_bool(frame != nullptr));
	if (frame) {
		sights.set("card", JsonValue::make_bool(frame->scope_card_active));
		sights.set("scoped", JsonValue::make_bool(frame->frame_fx.scoped_selector));
		sights.set("sighted", JsonValue::make_bool(frame->frame_fx.sighted_selector));
		sights.set("readouts", JsonValue::make_bool(frame->scope_details_active));
		sights.set("range", json_number(double(frame->aim_range_q16) / 65536.0));
		sights.set("zero", json_number(frame->scope_zero_word));
		sights.set("magnification", json_number(frame->scope_magnification));
		sights.set("fov", json_number(frame->fov_h_deg));
	}
	if (!scope_why_.empty()) sights.set("why", json_string(scope_why_));
	out.set("sights", std::move(sights));
	return out;
}

io::JsonValue HudViewport::items_json(const ViewportInput &) const {
	JsonValue out = JsonValue::make_array();
	const HudPreviewElement *chosen = picked();
	for (const HudPreviewElement &element : elements_) {
		JsonValue row = JsonValue::make_object();
		row.set("element", json_string(opennova::hud::hud_element_token(element.element)));
		row.set("words", json_string(hud_element_words(element.element).words));
		JsonValue rect = JsonValue::make_array();
		for (const float v : { element.x0, element.y0, element.x1, element.y1 }) rect.push(json_number(v));
		row.set("rect", std::move(rect));
		JsonValue lines = JsonValue::make_array();
		for (const HudPreviewElement::Line &line : element.lines) {
			JsonValue at = JsonValue::make_object();
			at.set("key", json_string(line.key));
			at.set("line", json_number(double(line.line)));
			at.set("column", json_number(double(line.column)));
			at.set("locator", json_string(TextDocument::locator(line.line, line.column)));
			at.set("text", json_string(line.text));
			lines.push(std::move(at));
		}
		row.set("lines", std::move(lines));
		JsonValue textures = JsonValue::make_array();
		for (const HudPreviewElement::Art &art : element.textures) {
			JsonValue texture = JsonValue::make_object();
			texture.set("name", json_string(art.name));
			texture.set("path", json_string(art.path));
			textures.push(std::move(texture));
		}
		row.set("textures", std::move(textures));
		row.set("picked", JsonValue::make_bool(&element == chosen));
		row.set("movable", JsonValue::make_bool(element.movable));
		row.set("resizable", JsonValue::make_bool(element.resizable));
		JsonValue fields = JsonValue::make_array();
		if (model_)
			for (const HudField &field : hud_element_fields(element.element, model_->file.hud)) {
				JsonValue each = JsonValue::make_object();
				each.set("id", json_string(field.id));
				each.set("words", json_string(field.words));
				each.set("kind", json_string(field.kind == HudFieldKind::Number  ? "number"
				                             : field.kind == HudFieldKind::Align ? "align"
				                                                                 : "name"));
				each.set("value", json_string(field.value));
				if (field.kind == HudFieldKind::Number) {
					each.set("least", json_number(field.least));
					each.set("most", json_number(field.most));
				}
				each.set("range", json_string(field.range));
				each.set("cite", json_string(field.cite));
				fields.push(std::move(each));
			}
		row.set("fields", std::move(fields));
		out.push(std::move(row));
	}
	return out;
}

} // namespace opennova::editor
