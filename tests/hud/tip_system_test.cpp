// The tip system ("MrClippy", hud/tip_system.h): the event table with its
// once-counters and option gates, the countdown / fade / reset, the text keys
// and the macro expansion, the escape chain's tip leg, the panel's draw, the
// "$token$" key display, and the world's producers (the local player's
// boarding and detach).
// [orig: CTipSystem_HandleEvent @0x5b6ad0; CTipSystem_ShowTip @0x5b6a60;
//  CTipSystem_TickCountdown @0x5b69f0; CTipSystem_IsShowing @0x5b6c60;
//  CTipSystem_BeginFade @0x5b6c70; CTipSystem_Reset @0x5b6940; CTipSystem_Draw
//  @0x5b6d60; TextResource_ExpandMacroVariables @0x5b6c80;
//  KeyBinding_GetDisplayStringByActionName @0x5b6a00; Input_HandleActionBinding
//  case 18 @0x49b34d; Entity_ProcessVehicleAttach @0x435bfd..0x435cc0;
//  Entity_DetachFromVehicle @0x4356b0]
#include <runtime/audio/oneshot_play.h>
#include <runtime/controls/binding_set.h>
#include <runtime/hud/hud_frame.h>
#include <runtime/hud/hud_math.h>
#include <runtime/hud/hud_toggles.h>
#include <runtime/hud/tip_system.h>
#include <runtime/world/vehicle_attach.h>
#include <runtime/world/world.h>
#include <formats/lwf/lwf.h>

#include "common/test_font.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace opennova;
using namespace opennova::hud;

namespace {

int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

// The event table [orig: @0x5b6adc]: 1..5 the boarding tips at 620 frames.
void test_event_table() {
	const std::pair<int, int32_t> boarding[] = {
			{kTipEventBoardGround, kTipGround}, {kTipEventBoardGroundHorn, kTipGroundHorn},
			{kTipEventBoardHelo, kTipHelo}, {kTipEventBoardBoat, kTipBoat},
			{kTipEventBoardEmplaced, kTipEmplaced}};
	for (const auto &[event, tip] : boarding) {
		TipSystem t;
		tip_handle_event(t, event);
		CHECK(t.tip == tip && t.countdown == kTipKeyboardFrames);
	}
	// 22: the spectator tip.
	{
		TipSystem t;
		tip_handle_event(t, kTipEventSpectatorBegin);
		CHECK(t.tip == kTipSpectatorBegin && t.countdown == 620);
	}
	// Every fade event clamps the countdown to 64 and keeps the id.
	for (const int fade : {kTipEventDetach, kTipEventNvgOff, kTipEventBinocularsOff,
				 kTipEventScopeElevationOff, kTipEventDesignatorWeaponOff,
				 kTipEventDesignatorOff}) {
		TipSystem t;
		tip_handle_event(t, kTipEventBoardHelo);
		tip_handle_event(t, fade);
		CHECK(t.tip == kTipHelo && t.countdown == kTipFadeFrames);
		t.countdown = 10; // a shorter countdown is left alone
		tip_handle_event(t, fade);
		CHECK(t.countdown == 10);
	}
	// 17..21 (the game-event handler's raises) and unknown ids are no-ops.
	for (const int event : {0, 17, 18, 19, 20, 21, 23, 99}) {
		TipSystem t;
		tip_handle_event(t, event);
		CHECK(t.tip == 0 && t.countdown == 0);
	}
}

// The once-counters: NVG and binoculars once, the designator pair twice, the
// scope four times alternating the elevation tip (keyboard, 620) and the
// binocular hint (gameplay, 1240) [orig: @0x5b6b31..0x5b6bda].
void test_once_counters() {
	TipSystem t;
	tip_handle_event(t, kTipEventNvgOn);
	CHECK(t.tip == kTipNvg && t.nvg_count == 1);
	t.tip = 0;
	tip_handle_event(t, kTipEventNvgOn);
	CHECK(t.tip == 0 && t.nvg_count == 1);
	tip_handle_event(t, kTipEventBinocularsOn);
	CHECK(t.tip == kTipBinocRange && t.countdown == kTipGameplayFrames);
	t.tip = 0;
	tip_handle_event(t, kTipEventBinocularsOn);
	CHECK(t.tip == 0 && t.binoculars_count == 1);
	const int32_t scope[] = {kTipScopeElevation, kTipScopeUseBinoc, kTipScopeElevation,
			kTipScopeUseBinoc};
	for (const int32_t want : scope) {
		t.tip = 0;
		tip_handle_event(t, kTipEventScopeElevationOn);
		CHECK(t.tip == want);
		CHECK(t.countdown == (want == kTipScopeElevation ? 620 : 1240));
	}
	t.tip = 0;
	tip_handle_event(t, kTipEventScopeElevationOn);
	CHECK(t.tip == 0 && t.scope_count == 4);
	for (int i = 0; i < 3; ++i) {
		t.tip = 0;
		tip_handle_event(t, kTipEventDesignatorWeaponOn);
		CHECK(t.tip == (i < 2 ? kTipMortarUseDesignator : 0));
		t.tip = 0;
		tip_handle_event(t, kTipEventDesignatorOn);
		CHECK(t.tip == (i < 2 ? kTipDesignatorHelpMortar : 0));
	}
	CHECK(t.designator_weapon_count == 2 && t.designator_count == 2);
}

// The option gates: a refused tip writes nothing, but its once-counter still
// counts [orig: the gate return @0x5b6ac3; the increments follow the call].
void test_option_gates() {
	TipSystem t;
	t.options.keyboard_tips = false;
	tip_handle_event(t, kTipEventBoardGround);
	CHECK(t.tip == 0 && t.countdown == 0);
	tip_handle_event(t, kTipEventNvgOn);
	CHECK(t.tip == 0 && t.nvg_count == 1);
	tip_handle_event(t, kTipEventBinocularsOn); // gameplay tips still on
	CHECK(t.tip == kTipBinocRange && t.countdown == 1240);
	t.options.gameplay_tips = false;
	t.tip = 0;
	t.countdown = 0;
	tip_handle_event(t, kTipEventDesignatorOn);
	CHECK(t.tip == 0 && t.designator_count == 1);
	// A fade is never gated.
	t.countdown = 500;
	tip_handle_event(t, kTipEventDetach);
	CHECK(t.countdown == 64);
}

void test_countdown_fade_reset() {
	TipSystem t;
	tip_handle_event(t, kTipEventBoardBoat);
	CHECK(tip_is_showing(t));
	for (int i = 0; i < 620 - 65; ++i) tip_tick_countdown(t);
	CHECK(t.countdown == 65 && tip_is_showing(t));
	tip_tick_countdown(t);
	CHECK(t.countdown == 64 && !tip_is_showing(t));
	for (int i = 0; i < 100; ++i) tip_tick_countdown(t);
	CHECK(t.countdown == 0); // floored
	tip_handle_event(t, kTipEventBoardBoat);
	tip_begin_fade(t);
	CHECK(t.countdown == 64 && t.tip == kTipBoat);
	// The SP restart clears the tip, a start from the menu the counters too
	// [orig: Game_StartMission @0x525dec -- CTipSystem_Reset(first start)].
	t.nvg_count = 1;
	t.scope_count = 3;
	tip_reset(t, false);
	CHECK(t.tip == 0 && t.countdown == 0 && t.nvg_count == 1 && t.scope_count == 3);
	tip_reset(t, true);
	CHECK(t.nvg_count == 0 && t.scope_count == 0 && t.binoculars_count == 0 &&
			t.designator_weapon_count == 0 && t.designator_count == 0);
}

void test_text_keys_and_alpha() {
	TipText k;
	CHECK(tip_text_keys(kTipGround, k) && std::strcmp(k.header_key, "StrKBTip") == 0 &&
			std::strcmp(k.body_key, "KB_GROUND") == 0 && !k.gameplay);
	CHECK(tip_text_keys(kTipSpectatorBegin, k) &&
			std::strcmp(k.body_key, "KB_SPECTATORBEGIN") == 0);
	CHECK(tip_text_keys(kTipBinocRange, k) && std::strcmp(k.header_key, "StrGPTip") == 0 &&
			std::strcmp(k.body_key, "GP_BINOC_RANGE") == 0 && k.gameplay);
	CHECK(tip_text_keys(kTipAasBegin, k) && std::strcmp(k.body_key, "GP_AAS_BEGIN") == 0);
	for (const int32_t skipped : {0, 1, 10, 11, 19, 20})
		CHECK(!tip_text_keys(skipped, k));
	CHECK(tip_alpha(620) == 255 && tip_alpha(64) == 255 && tip_alpha(63) == 252 &&
			tip_alpha(10) == 40 && tip_alpha(0) == 0);
}

// The expansion [orig: @0x5b6caa..0x5b6d34].
void test_expand_macros() {
	const TipKeyDisplay keys = [](const std::string &token) {
		if (token == "move_forward") return std::string("W");
		if (token == "useitem") return std::string("E");
		return std::string("???");
	};
	CHECK(tip_expand_macros("'$move_forward$'\\t- Forward\r\n", keys) == "'W'\t- Forward\r\n");
	CHECK(tip_expand_macros("a\\nb\\x", keys) == "a\nb\\x");
	CHECK(tip_expand_macros("$useitem$$nope$", keys) == "E???");
	// An unterminated name swallows the rest of the text as the name.
	CHECK(tip_expand_macros("x $useitem", keys) == "x E");
	CHECK(tip_expand_macros("x $use", keys) == "x ???");
	CHECK(tip_expand_macros("\\", keys) == "\\");
	std::string header;
	std::string body;
	const GameTextLookup table = [](const char *section, const char *key, const char *fallback) {
		if (std::strcmp(section, "Tips") == 0 && std::strcmp(key, "KB_EMPLACED") == 0)
			return std::string("'$useitem$'\\t- Detach");
		return std::string(fallback);
	};
	CHECK(tip_draw_text(kTipEmplaced, table, keys, header, body));
	CHECK(header == "StrKBTip" && body == "'E'\t- Detach");
	CHECK(!tip_draw_text(0, table, keys, header, body) && header.empty() && body.empty());
}

// The key display by token: case-insensitive, "???" on a miss
// [orig: KeyBinding_GetDisplayStringByActionName @0x5b6a00].
void test_display_string_for_token() {
	const controls::BindingSet set;
	const std::string forward = controls::display_string_for_token(set, "move_forward");
	CHECK(!forward.empty() && forward != "???");
	CHECK(controls::display_string_for_token(set, "MOVE_FORWARD") == forward);
	CHECK(controls::display_string_for_token(set, "no_such_action") == "???");
}

// The escape chain's tip leg: after the map legend, before the in-game menu
// [orig: @0x49b32d / @0x49b34d / @0x49b36a].
void test_escape_tip_leg() {
	using namespace hud_toggle_event;
	HudToggleState s;
	HudEscapeInput in;
	tip_handle_event(s.tips, kTipEventBoardHelo);
	s.map_legend_open = true;
	CHECK(hud_toggles_escape(s, in) == (kEscapeClosedWindow | kOverlayWindowsCleared));
	CHECK(!s.map_legend_open && s.tips.countdown == 620);
	CHECK(hud_toggles_escape(s, in) == kEscapeClosedWindow);
	CHECK(s.tips.countdown == 64 && s.tips.tip == kTipHelo);
	// A fading tip no longer takes the key.
	CHECK(hud_toggles_escape(s, in) == (kEscapeOpenMenu | kOverlayWindowsCleared));
	// The drain and the frames.
	const uint8_t events[] = {kTipEventNvgOn, kTipEventBinocularsOn};
	hud_toggles_tip_events(s, events, 2);
	CHECK(s.tips.tip == kTipBinocRange && s.tips.nvg_count == 1);
	hud_toggles_tip_frames(s, 40);
	CHECK(s.tips.countdown == 1200);
	hud_toggles_tip_frames(s, 1000000);
	CHECK(s.tips.countdown == 0);
	// The SP restart keeps the counters; a start from the menu clears them
	// [orig: CTipSystem_Reset @0x525df2 -- 0 on Game_StartMission(1), 1 on
	//  Game_StartMission(0)].
	tip_handle_event(s.tips, kTipEventBoardBoat);
	hud_toggles_restart_round(s);
	CHECK(s.tips.tip == 0 && s.tips.countdown == 0 && s.tips.nvg_count == 1);
	hud_toggles_reset_mission(s);
	CHECK(s.tips.nvg_count == 0 && s.tips.binoculars_count == 0);
}

std::vector<HudQuad> quads_with(const HudDrawList &list, int32_t texture) {
	std::vector<HudQuad> out;
	for (const HudQuad &q : list.quads)
		if (q.texture == texture) out.push_back(q);
	return out;
}

// The panel [orig: CTipSystem_Draw @0x5b6d60]: at 1024x768 design space is
// the surface, so the rect is the hudpos slot plus the measured text.
void test_draw(const fnt::fnt_font_t *font) {
	HudLayout layout;
	layout.tip_normal = {8, 100, 64, 108};
	layout.tip_alternate = {8, 340, 64, 108};
	layout.tip_box_texture_valid = true;
	layout.tip_box_tex_w = 128;
	layout.tip_keyboard_texture_valid = true;
	layout.tip_gameplay_texture_valid = true;
	HudFrameCompiler compiler;
	compiler.configure(layout, font);
	compiler.configure_label_fonts(font, font, font, 1.0f, 1.0f);
	HudFrameState state;
	state.tip = kTipNvg;
	state.tip_countdown = 0;
	state.tip_header = "KT";
	state.tip_body = "B";
	CHECK(quads_with(compiler.compile(state, 1024.0f, 768.0f), kHudTexTipBox).empty());
	state.tip_countdown = 620;
	{
		const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
		const std::vector<HudQuad> box = quads_with(list, kHudTexTipBox);
		// One fill of cell (1,1) then the eight pieces.
		CHECK(box.size() == 9);
		if (box.size() == 9) {
			CHECK(box[0].u0 == 0.25f && box[0].v0 == 0.25f && box[0].u1 == 0.5f &&
					box[0].v1 == 0.5f);
			// The fill insets one 32-px cell (scale 1024/1024) from the rect.
			CHECK(box[0].x0 == 8.0f + 32.0f && box[0].y0 == 100.0f + 32.0f);
			// alpha 255, the raw 0x7F7F7F diffuse the box material's
			// MODULATE2X doubles on the device (D-HUD-49).
			CHECK(box[0].color == 0xFF7F7F7Fu);
			// The top-left piece sits at the slot origin.
			CHECK(box[1].x0 == 8.0f && box[1].y0 == 100.0f);
		}
		const std::vector<HudQuad> icon = quads_with(list, kHudTexTipKeyboard);
		CHECK(icon.size() == 1);
		if (!icon.empty()) {
			const auto near = [](float a, float b) { return a > b - 0.01f && a < b + 0.01f; };
			CHECK(near(icon[0].x0, 36.0f) && near(icon[0].y0, 132.0f) &&
					near(icon[0].x1, 84.0f) && near(icon[0].y1, 180.0f));
			// The raw grey 0x80 the icon's material 0x651 doubles on the device.
			CHECK(icon[0].color == 0xFF808080u);
		}
		CHECK(quads_with(list, kHudTexTipGameplay).empty());
		// The header at (88, 142) orange, the body at (36, 192) white (the
		// glyph quads carry the drawer's half-pixel bias), both
		// through the alpha-keeping half-bright fold.
		bool header = false;
		bool body = false;
		for (const GameFontQuad &g : list.glyphs) {
			if (g.color == half_bright_keep_alpha(0xFFEEA400u) && g.x_top_left >= 87.0f &&
					g.x_top_left < 100.0f)
				header = true;
			if (g.color == half_bright_keep_alpha(0xFFFFFFFFu) && g.x_top_left >= 35.0f &&
					g.x_top_left < 48.0f)
				body = true;
		}
		if (!(header && body))
			for (const GameFontQuad &g : list.glyphs)
				std::printf("glyph x %.1f y %.1f color %08X\n", g.x_top_left, g.y_top, g.color);
		CHECK(header && body);
	}
	// Fading: alpha 4 * countdown.
	state.tip_countdown = 10;
	{
		const std::vector<HudQuad> box =
				quads_with(compiler.compile(state, 1024.0f, 768.0f), kHudTexTipBox);
		CHECK(!box.empty() && (box[0].color >> 24) == 40u);
	}
	// A gameplay tip draws the g_tip icon.
	state.tip = kTipBinocRange;
	state.tip_countdown = 1240;
	CHECK(quads_with(compiler.compile(state, 1024.0f, 768.0f), kHudTexTipGameplay).size() == 1);
	// The M-cycle map moves it to the alternate slot, in the top layer; a
	// dead local player's map frame draws none.
	state.minimap.map_mode = 2;
	{
		const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
		const std::vector<HudQuad> box = quads_with(list, kHudTexTipBox);
		CHECK(box.size() == 9 && box[1].y0 == 340.0f);
		CHECK(list.top_begin.quads <= list.quads.size());
		bool in_top = !box.empty();
		for (size_t i = 0; i < list.top_begin.quads && i < list.quads.size(); ++i)
			if (list.quads[i].texture == kHudTexTipBox) in_top = false;
		CHECK(in_top);
	}
	state.local_dead = true;
	CHECK(quads_with(compiler.compile(state, 1024.0f, 768.0f), kHudTexTipBox).empty());
	// The blank HUD level keeps the tip (it rides the scene frame).
	state.local_dead = false;
	state.minimap.map_mode = 0;
	state.hud_detail_level = 3;
	CHECK(quads_with(compiler.compile(state, 1024.0f, 768.0f), kHudTexTipBox).size() == 9);
}

world::Entity make_player() {
	world::Entity e;
	e.kind = world::EntityKind::Organic;
	return e;
}

world::Entity make_vehicle(int32_t item_id, int32_t unit_type, world::SeatType seat_type) {
	world::Entity e;
	e.kind = world::EntityKind::Item;
	e.item_id = item_id;
	e.has_item_def = true;
	// Control seats need PlayerControl [orig: Entity_AttachToVehicleSlot @0x4947cc].
	e.item_attrib = world::kItemAttribPlayerControl;
	e.item_type = 1;
	e.item_unit_type = unit_type;
	world::Seat seat;
	seat.type = seat_type;
	seat.bone_index = 7;
	e.seats.push_back(seat);
	return e;
}

// The boarding tips by seat and def, the horn slot, and the detach fade — for
// the local player only [orig: Entity_ProcessVehicleAttach @0x435bfd..0x435cc0;
// Entity_DetachFromVehicle @0x4356b0].
void test_world_boarding() {
	using world::SeatType;
	auto fixture = std::make_unique<world::World>();
	world::World &w = *fixture;
	w.registry.configure_pool(0, 16);
	w.registry.configure_pool(1, 16);
	static const char kProfiles[] =
			"begin \"default\"\r\nend\r\n"
			"begin \"SP_Truck\"\r\n ssaudio1\tV_HORN\r\nend\r\n"
			"begin \"SP_Quiet\"\r\n ssaudio1\tV_MISSING\r\nend\r\n";
	w.tables.sound_profiles.parse(kProfiles, sizeof(kProfiles) - 1);
	lwf::File bank;
	bank.multis.resize(1);
	bank.multis[0].name = "V_HORN";
	audio::SoundSetIndex sets;
	sets.add_bank(0, bank);
	w.tables.sound_sets = &sets;
	world::ItemDeathTraits truck;
	truck.sound_profile = "SP_Truck";
	w.tables.item_death_traits.set(10, truck);
	world::ItemDeathTraits quiet;
	quiet.sound_profile = "SP_Quiet";
	w.tables.item_death_traits.set(11, quiet);

	const world::EntityHandle local = w.registry.spawn(0, make_player());
	const world::EntityHandle other = w.registry.spawn(0, make_player());
	w.cached.local_player = local;

	struct Case {
		int32_t item_id;
		int32_t unit_type;
		SeatType seat;
		int event;
	};
	const Case cases[] = {
			{10, 1, SeatType::Driver, kTipEventBoardGroundHorn},
			{11, 1, SeatType::Driver, kTipEventBoardGround},
			{12, 2, SeatType::Controller, kTipEventBoardGround}, // no traits: "default"
			{10, 3, SeatType::Controller, kTipEventBoardHelo},
			{10, 5, SeatType::Driver, kTipEventBoardBoat},
			{10, 8, SeatType::Driver, kTipEventBoardBoat},
			{10, 4, SeatType::Driver, kTipEventBoardGroundHorn},
			{10, 1, SeatType::Gunner, kTipEventBoardEmplaced},
			{10, 1, SeatType::Passenger, 0},
	};
	for (const Case &c : cases) {
		const world::EntityHandle veh =
				w.registry.spawn(1, make_vehicle(c.item_id, c.unit_type, c.seat));
		CHECK(world::vehicle_boarding_tip_event(w, *w.registry.get(veh), c.seat) == c.event);
		w.out.tip_events.clear();
		CHECK(w.vehicles.process_attach(local, veh, 7));
		const std::vector<uint8_t> want =
				c.event != 0 ? std::vector<uint8_t>{static_cast<uint8_t>(c.event)}
							 : std::vector<uint8_t>{};
		CHECK(w.out.tip_events == want);
		w.out.tip_events.clear();
		CHECK(w.vehicles.detach(local));
		CHECK(w.out.tip_events == std::vector<uint8_t>{kTipEventDetach});
		w.out.tip_events.clear();
		// Another player's boarding raises nothing.
		CHECK(w.vehicles.process_attach(other, veh, 7));
		CHECK(w.vehicles.detach(other));
		CHECK(w.out.tip_events.empty());
		w.registry.despawn(veh);
	}
	// A control seat the carrier refuses (its def lacks PlayerControl) raises
	// no tip, on the authority or a client (D-NET-398).
	// [orig: Entity_AttachToVehicleSlot @0x4948d2; Entity_ProcessVehicleAttach
	//  @0x435c09 -> @0x435c67]
	world::Entity locked = make_vehicle(10, 1, SeatType::Driver);
	locked.item_attrib = 0;
	const world::EntityHandle locked_h = w.registry.spawn(1, locked);
	w.out.tip_events.clear();
	CHECK(!w.vehicles.process_attach(local, locked_h, 7));
	CHECK(!w.vehicles.apply_confirmed_mount(local, locked_h, 7));
	CHECK(w.out.tip_events.empty() && !w.registry.get(local)->mounted);
	w.registry.despawn(locked_h);
	// A non-vehicle def's control seat raises no vehicle tip.
	world::Entity crate = make_vehicle(10, 1, SeatType::Driver);
	crate.item_type = 2;
	CHECK(world::vehicle_boarding_tip_event(w, crate, SeatType::Driver) == 0);
	// The client's confirmed mount raises the same tips.
	const world::EntityHandle heli =
			w.registry.spawn(1, make_vehicle(10, 3, SeatType::Controller));
	w.out.tip_events.clear();
	CHECK(w.vehicles.apply_confirmed_mount(local, heli, 7));
	CHECK(w.out.tip_events == std::vector<uint8_t>{kTipEventBoardHelo});
	w.out.tip_events.clear();
	CHECK(w.vehicles.apply_confirmed_mount(local, world::EntityHandle{}, 0));
	CHECK(w.out.tip_events == std::vector<uint8_t>{kTipEventDetach});
	w.tables.sound_sets = nullptr;
}

} // namespace

int main() {
	test_event_table();
	test_once_counters();
	test_option_gates();
	test_countdown_fade_reset();
	test_text_keys_and_alpha();
	test_expand_macros();
	test_display_string_for_token();
	test_escape_tip_leg();
	fnt::fnt_font_t font = test_font::uniform_test_font();
	test_draw(&font);
	fnt::fnt_free(&font);
	test_world_boarding();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("tip_system_test OK\n");
	return 0;
}
