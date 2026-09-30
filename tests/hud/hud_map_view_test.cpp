// The windowed map views (hud/hud_map_view.h): the DEATH MAP window's and the
// CMAP map's pan/zoom state machines (the witnessed event maps, the pan
// factor K = Z x 65536 x S / (200 x W), the 1.005^(-dx) drag zoom, the
// x0.85 / x1.1764705 steps, the [0.1, 10] clamp, the show-event fit and the
// per-frame pan ease), the CMAP mode-4 mask / compile input, and the DEATH
// pass compile (zone letters, their colours and score lines, the crosshair).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <runtime/hud/hud_map_view.h>

using namespace opennova::hud;

static int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

constexpr int32_t kWu = 65536;

bool near(double a, double b, double eps) {
    return std::fabs(a - b) <= eps;
}

// The DEATH MAP widget at 800x600 (scale 1): the authored MAP (10,10)-(540,470)
// inside DEATH_SHROUD (20,20)-(780,545).
MapViewRect death_rect() {
    MapViewRect r;
    r.left = 30;
    r.top = 30;
    r.right = 560;
    r.bottom = 490;
    return r;
}

void test_integer_pow() {
    CHECK(map_view_integer_pow(1.005, 0) == 1.0);
    CHECK(map_view_integer_pow(2.0, 10) == 1024.0);
    CHECK(map_view_integer_pow(2.0, -2) == 0.25);
    // 1.005^-10 at single precision.
    CHECK(near(map_view_integer_pow(1.005, -10), 0.9513, 1e-4));
    CHECK(map_view_integer_pow(1.005, -10) == static_cast<double>(
            static_cast<float>(map_view_integer_pow(1.005, -10))));
}

void test_design_to_device() {
    // The mouse leg's inverse: the device point over the scale, truncated.
    CHECK(map_view_device_to_design(800, 1.0f) == 800);
    CHECK(map_view_device_to_design(72, 2.0f) == 36);
    // 72 / 2.4f lands a hair under 30 in single precision and truncates.
    CHECK(map_view_device_to_design(72, 2.4f) == 29);
    CHECK(map_view_device_to_design(1365, 1366.0f / 800.0f) == 799);
    CHECK(map_view_device_to_design(0, 0.0f) == 0);
    CHECK(map_view_design_to_device(800, 1.0f) == 800);
    CHECK(map_view_design_to_device(30, 2.4f) == 72);
    // The product rounds to single precision (the x87 precision control)
    // before the truncation: 800 x (1366 / 800) is 1366 again.
    CHECK(map_view_design_to_device(800, 1366.0f / 800.0f) == 1366);
    CHECK(map_view_design_to_device(799, 1366.0f / 800.0f) == 1364);
}

void test_death_load() {
    DeathMapView v;
    v.on_load();
    CHECK(v.initialized);
    CHECK(v.view.zoom == 4.0f);
    v.view.zoom = 2.0f;
    v.view.pan_x = 5;
    v.on_unload();
    CHECK(!v.initialized);
    v.on_load();
    // A later load keeps the held zoom and pan (the seed keys on zoom == 0).
    CHECK(v.view.zoom == 2.0f);
    CHECK(v.view.pan_x == 5);
}

void test_death_render_and_pan() {
    DeathMapView v;
    v.on_load();
    const DeathMapFrame f = v.render(death_rect(), 800, 100 * kWu, -50 * kWu);
    CHECK(v.view.window_w == 530);
    CHECK(v.view.window_h == 460);
    CHECK(v.view.pixel_ratio == 1.0f);
    CHECK(f.center_x == 100 * kWu);
    CHECK(f.center_y == -50 * kWu);
    // 4 x 65536 / (200 x 530) at single precision.
    CHECK(f.scale == static_cast<float>(262144.0 / 106000.0));
    // Left drag 10 px right, 5 down: K = 2.4730566; x -= 10K x 65536,
    // y += 5K x 65536 (each product at single precision, then truncated),
    // on the pan and the eased target alike.
    CHECK(!v.on_event(MapViewEvent::kLeftDown, 200, 200, 0, 0));
    CHECK(v.view.left_drag && !v.view.right_drag);
    CHECK(v.on_event(MapViewEvent::kMove, 210, 205, kMapViewButtonLeft, 0));
    CHECK(v.view.pan_x == -1620742);
    CHECK(v.view.pan_y == 810371);
    CHECK(v.target_x == v.view.pan_x);
    CHECK(v.target_y == v.view.pan_y);
    CHECK(v.view.last_x == 210 && v.view.last_y == 205);
    // A move without the latched button clears both latches.
    CHECK(v.on_event(MapViewEvent::kMove, 220, 205, 0, 0));
    CHECK(!v.view.left_drag && !v.view.right_drag);
    // The pan follows the drag direction in the view: centre moves opposite
    // to the mouse on x, with it on y (mission +Y is up the screen).
    const DeathMapFrame g = v.render(death_rect(), 800, 100 * kWu, -50 * kWu);
    CHECK(g.center_x < f.center_x);
    CHECK(g.center_y > f.center_y);
}

void test_death_zoom() {
    DeathMapView v;
    v.on_load();
    v.render(death_rect(), 800, 0, 0);
    // Right drag 10 px right: zoom x= 1.005^-10.
    CHECK(!v.on_event(MapViewEvent::kRightDown, 100, 100, 0, 0));
    CHECK(!v.view.left_drag && v.view.right_drag);
    v.on_event(MapViewEvent::kMove, 110, 100, kMapViewButtonRight, 0);
    CHECK(near(v.view.zoom, 4.0 * std::pow(1.005, -10.0), 1e-4));
    // A long drag left caps at 10, a long drag right floors at 0.1.
    v.on_event(MapViewEvent::kMove, -2000, 100, kMapViewButtonRight, 0);
    CHECK(v.view.zoom == 10.0f);
    v.on_event(MapViewEvent::kMove, 3000, 100, kMapViewButtonRight, 0);
    CHECK(v.view.zoom == 0.1f);
    v.on_event(MapViewEvent::kRightUp, 0, 0, 0, 0);
    CHECK(!v.view.right_drag);
    // The wheel: forward zooms in x0.85, back out x1.1764705; each side
    // clamps one way only.
    v.view.zoom = 4.0f;
    v.on_event(MapViewEvent::kWheel, 0, 0, 0, 120);
    CHECK(v.view.zoom == static_cast<float>(4.0 * static_cast<double>(0.85f)));
    v.on_event(MapViewEvent::kWheel, 0, 0, 0, -120);
    CHECK(near(v.view.zoom, 4.0, 1e-5));
    v.view.zoom = 0.11f;
    v.on_event(MapViewEvent::kWheel, 0, 0, 0, 1);
    CHECK(v.view.zoom == 0.1f);
    v.view.zoom = 9.0f;
    v.on_event(MapViewEvent::kWheel, 0, 0, 0, -1);
    CHECK(v.view.zoom == 10.0f);
    // The left press drops the right drag, the right press the left one.
    v.on_event(MapViewEvent::kRightDown, 0, 0, 0, 0);
    v.on_event(MapViewEvent::kLeftDown, 0, 0, 0, 0);
    CHECK(v.view.left_drag && !v.view.right_drag);
}

void test_death_fit_and_ease() {
    DeathMapView v;
    v.on_load();
    DeathMapFitInput in;
    in.shroud_present = true;
    in.shroud_w = 760;
    in.shroud_h = 525;
    in.bounds_min_x = -100 * kWu;
    in.bounds_min_y = -50 * kWu;
    in.bounds_max_x = 300 * kWu;
    in.bounds_max_y = 150 * kWu;
    in.player_x = 0;
    in.player_y = 0;
    v.fit(in);
    // extent 400 wu over 760 x 0.4 design px, x 1.5.
    CHECK(near(v.view.zoom, 400.0 / (760.0 * 0.4) * 1.5, 1e-5));
    CHECK(v.view.pan_x == 100 * kWu);
    CHECK(v.view.pan_y == 50 * kWu);
    CHECK(v.target_x == 0 && v.target_y == 0);
    // The ease pulls the pan one eighth of the way to the target per frame.
    v.ease_frame();
    CHECK(v.view.pan_x == 100 * kWu - (100 * kWu >> 3));
    CHECK(v.view.pan_y == 50 * kWu - (50 * kWu >> 3));
    for (int i = 0; i < 400; ++i) v.ease_frame();
    CHECK(v.view.pan_x <= 7 && v.view.pan_x >= 0);
    // The player widens the AABB by 20 wu each side.
    in.player_x = 400 * kWu;
    v.fit(in);
    CHECK(v.view.pan_x == ((-100 * kWu + 420 * kWu) >> 1) - 400 * kWu);
    // All-zero bounds: extent 0 -> zoom 4, pan 0 (no widening).
    DeathMapFitInput zero;
    zero.shroud_present = true;
    zero.shroud_w = 760;
    zero.shroud_h = 525;
    zero.player_x = 50 * kWu;
    v.fit(zero);
    CHECK(v.view.zoom == 4.0f);
    CHECK(v.view.pan_x == 0 && v.view.pan_y == 0);
    // A fit that clamps to 10 zeroes the pan and the target.
    DeathMapFitInput wide = in;
    wide.bounds_max_x = 30000 * kWu;
    v.target_x = 77;
    v.fit(wide);
    CHECK(v.view.zoom == 10.0f);
    CHECK(v.view.pan_x == 0 && v.target_x == 0);
    // A fit below 0.1 floors (the +-20 wu widening keeps 40 wu; a large
    // shroud side carries it under).
    DeathMapFitInput tiny = in;
    tiny.shroud_w = 2000;
    tiny.bounds_min_x = -1;
    tiny.bounds_min_y = 0;
    tiny.bounds_max_x = 1;
    tiny.bounds_max_y = 0;
    tiny.player_x = 0;
    v.fit(tiny);
    CHECK(v.view.zoom == 0.1f);
    // Without the shroud window only the 10.0 test runs.
    DeathMapFitInput none;
    v.view.zoom = 10.0f;
    v.view.pan_x = 9;
    v.fit(none);
    CHECK(v.view.pan_x == 0);
    v.view.zoom = 3.0f;
    v.view.pan_x = 9;
    v.fit(none);
    CHECK(v.view.pan_x == 9 && v.view.zoom == 3.0f);
}

void test_command_map() {
    CommandMapView c;
    c.on_load();
    CHECK(c.view.zoom == 4.0f);
    CHECK(c.toggles.grid && c.toggles.text && c.toggles.waypoints);
    CHECK(command_map_mask(c.toggles) == 0xAF937u);
    CommandMapToggles t = c.toggles;
    t.grid = false;
    CHECK(command_map_mask(t) == 0xAE937u);
    t = c.toggles;
    t.text = false;
    CHECK(command_map_mask(t) == 0x8B137u);
    t = c.toggles;
    t.waypoints = false;
    CHECK(command_map_mask(t) == 0x8F937u);
    // The wheel is ignored while the right drag runs.
    c.on_event(MapViewEvent::kRightDown, 10, 10, 0, 0);
    c.on_event(MapViewEvent::kWheel, 10, 10, 0, 1);
    CHECK(c.view.zoom == 4.0f);
    // The left press does not drop the right latch here.
    c.on_event(MapViewEvent::kLeftDown, 10, 10, 0, 0);
    CHECK(c.view.left_drag && c.view.right_drag);
    c.on_event(MapViewEvent::kRightUp, 10, 10, 0, 0);
    c.on_event(MapViewEvent::kWheel, 10, 10, 0, 1);
    CHECK(c.view.zoom == static_cast<float>(4.0 * static_cast<double>(0.85f)));
    // CREATE_WAYPOINTS routes the left press away from the pan.
    CommandMapView w;
    w.on_load();
    w.toggles.create_waypoints = true;
    w.on_event(MapViewEvent::kLeftDown, 10, 10, 0, 0);
    CHECK(!w.view.left_drag);
    // The buttons step the same way as the wheel.
    w.zoom_button(-1);
    CHECK(near(w.view.zoom, 4.0 * 1.1764705, 1e-5));
    // Render: mode 4, the virtual rect, the mask, the pan-shifted centre and
    // HUD_DrawMapOverlay's scale over the scaled width.
    HudMinimapInput in;
    in.surface_w = 1920.0f;
    in.surface_h = 1080.0f;
    MapViewRect r;
    r.left = 96;
    r.top = 90;
    r.right = 1104;
    r.bottom = 900;
    c.view.pan_x = 3 * kWu;
    c.render(r, 1920, 10 * kWu, 20 * kWu, 5 * kWu, in);
    CHECK(in.map_mode == 4);
    CHECK(in.flags == 0xAF937u);
    CHECK(in.player_x == 13 * kWu && in.player_y == 20 * kWu && in.player_z == 5 * kWu);
    CHECK(in.rect_x1 == 51.0f); // (96 x 1024 + 960) / 1920
    CHECK(in.rect_y1 == 64.0f); // (90 x 768 + 540) / 1080
    CHECK(c.view.window_w == 1008);
    CHECK(c.view.pixel_ratio == static_cast<float>(1920.0 * static_cast<double>(0.00125f)));
    CHECK(in.window_scale > 0.0f);
}

HudMinimapMarker zone_marker(uint16_t handle, int32_t x, int32_t y) {
    HudMinimapMarker m;
    m.bank = static_cast<uint8_t>(HudMinimapBank::kTransient);
    m.handle = handle;
    m.x = x;
    m.y = y;
    m.icon = 0;
    m.color = 0xFF304080u;
    m.entity_known = 1;
    return m;
}

bool has_label(const HudMapPass &pass, const char *text, float x, float y, uint32_t color) {
    for (const HudMapLabel &l : pass.labels) {
        if (std::strcmp(l.text, text) == 0 && l.x == x && l.y == y && l.color == color)
            return true;
    }
    return false;
}

void test_death_compile() {
    DeathMapView v;
    v.on_load();
    const int32_t cx = 100 * kWu;
    const int32_t cy = 200 * kWu;
    const DeathMapFrame frame = v.render(death_rect(), 800, cx, cy);
    DeathMapFacts facts;
    facts.player_present = true;
    facts.player_x = cx;
    facts.player_y = cy;
    facts.player_team = 1;
    facts.game_type = 0x30020;
    // Zone A (own team, the player's wave zone) at the view centre; zone B
    // (team 2) 20 wu east; zone C (own team) not secured -> no letter.
    DeathMapZone a;
    a.handle = 0x1001;
    a.index = 0;
    a.team = 1;
    a.anchor_x = cx;
    a.anchor_y = cy;
    a.queued = 2;
    a.countdown = 17;
    DeathMapZone b;
    b.handle = 0x1002;
    b.index = 1;
    b.team = 2;
    b.anchor_x = cx + 20 * kWu;
    b.anchor_y = cy;
    DeathMapZone c;
    c.handle = 0x1003;
    c.index = 2;
    c.team = 1;
    c.timer_ready = false;
    c.anchor_x = cx;
    c.anchor_y = cy - 10 * kWu;
    facts.zones = {a, b, c};
    facts.self_zone_handle = 0x1001;
    HudMinimapInput in;
    in.surface_w = 1024.0f;
    in.surface_h = 768.0f;
    in.ticks = 8; // pulse phase 0
    in.markers = {zone_marker(0x1001, a.anchor_x, a.anchor_y),
            zone_marker(0x1002, b.anchor_x, b.anchor_y),
            zone_marker(0x1003, c.anchor_x, c.anchor_y)};
    DeathMapCompiler compiler;
    HudMapWindowPass out;
    compiler.compile(in, frame, facts, out);
    CHECK(out.map.visible);
    // The window view: mode 4 north-up, the payload rect on 1024x768.
    CHECK(out.map.center_x == 295.0f && out.map.center_y == 260.0f);
    // Three walked blips in the pass, then the zone walk's own segments: each
    // zone's redrawn blip and that zone's letters, zone by zone.
    CHECK(out.map.sprites.size() == 3);
    CHECK(out.zones.sprites.size() == 3);
    CHECK(out.map.labels.empty());
    CHECK(out.segments.size() == 3);
    if (out.segments.size() == 3) {
        CHECK(out.segments[0].sprite_end == 1 && out.segments[0].label_end == 2);
        CHECK(out.segments[1].sprite_end == 2 && out.segments[1].label_end == 3);
        CHECK(out.segments[2].sprite_end == 3 && out.segments[2].label_end == 3);
    }
    // Zone A: the wave zone's colour, the letter at the anchor less half the
    // 8x16 cell, and its queued/countdown pair one cell below (no hold).
    CHECK(has_label(out.zones, "A", 291.0f, 252.0f, 0xFFC8C814u));
    CHECK(has_label(out.zones, "2/17", 291.0f, 268.0f, 0xFFC8C814u));
    // Zone B: the other team brightens 20/32 and has no score lines.
    const float bx = static_cast<float>(static_cast<int>(295.0f +
            20.0f / frame.scale - 4.0f));
    CHECK(has_label(out.zones, "B", bx, 252.0f, 0xFFCFABABu));
    // Zone C: timer not ready on a non-0x50010 game type -> no letter (its
    // blip still draws).
    for (const HudMapLabel &l : out.zones.labels) CHECK(std::strcmp(l.text, "C") != 0);
    CHECK(out.zones.labels.size() == 3);
    // The crosshair: two full-window lines through the player, team 1 blue at
    // alpha 0x70.
    CHECK(out.over_lines.size() == 2);
    if (out.over_lines.size() == 2) {
        CHECK(out.over_lines[0].x0 == 30.0f && out.over_lines[0].x1 == 560.0f);
        CHECK(out.over_lines[0].y0 == 260.0f && out.over_lines[0].y1 == 260.0f);
        CHECK(out.over_lines[1].y0 == 30.0f && out.over_lines[1].y1 == 490.0f);
        CHECK(out.over_lines[1].x0 == 295.0f);
        CHECK(out.over_lines[0].color == 0x704040FFu);
    }
    // The hold: own zones gain "%ld" a cell above; other zones stop
    // brightening.
    facts.hold_seconds = 9;
    compiler.compile(in, frame, facts, out);
    CHECK(has_label(out.zones, "9", 291.0f, 236.0f, 0xFFC8C814u));
    CHECK(has_label(out.zones, "B", bx, 252.0f, 0xFF802020u));
    // Game type 0x50010 letters every zone.
    facts.game_type = 0x50010;
    compiler.compile(in, frame, facts, out);
    bool has_c = false;
    for (const HudMapLabel &l : out.zones.labels) has_c = has_c || std::strcmp(l.text, "C") == 0;
    CHECK(has_c);
    // The deploy overlay suppresses the crosshair.
    facts.deploy_screen_active = true;
    compiler.compile(in, frame, facts, out);
    CHECK(out.over_lines.empty());
    // A special-bank slot is not a zone walk entry.
    in.markers[0].flags = 0x40;
    in.markers[0].remaining_ticks = 100;
    facts.game_type = 0x30020;
    facts.hold_seconds = 0;
    compiler.compile(in, frame, facts, out);
    for (const HudMapLabel &l : out.zones.labels) CHECK(std::strcmp(l.text, "A") != 0);
    // The pulse: an own-team zone at phase 16 moves half-way to white.
    in.markers[0].flags = 0;
    facts.self_zone_handle = 0xFFFF;
    in.ticks = 8 + 16;
    compiler.compile(in, frame, facts, out);
    CHECK(has_label(out.zones, "A", 291.0f, 252.0f, 0xFF979FBFu));
    // A def-type-2 zone draws no blip but keeps its letter segment.
    facts.zones[0].def_type = 2;
    compiler.compile(in, frame, facts, out);
    CHECK(out.zones.sprites.size() == 2);
    CHECK(out.segments.size() == 3 && out.segments[0].sprite_end == 0);
    // The stale 0x6E pair: the zone row carries whatever the fold last wrote.
    facts.zones[0].def_type = 0;
    facts.zones[0].queued = 0;
    facts.zones[0].countdown = 0;
    compiler.compile(in, frame, facts, out);
    CHECK(has_label(out.zones, "0/0", 291.0f, 268.0f, 0xFF979FBFu));
}

// sub_597FD0's colour / cell pick, branch by branch.
void test_zone_blip_pick() {
    DeathMapFacts facts;
    DeathMapZone z;
    // No def: nothing.
    z.has_def = false;
    CHECK(!death_map_zone_blip(z, facts, 0).draw);
    z.has_def = true;
    // Type 5 without attrib 0x20000: team colour or grey, cell 0.
    z.def_type = 5;
    z.team = 2;
    DeathMapZoneBlip b = death_map_zone_blip(z, facts, 0);
    CHECK(b.draw && b.color == 0xFF802020u && b.cell == 0);
    z.team = 3;
    CHECK(death_map_zone_blip(z, facts, 0).color == 0xFF707070u);
    // Type 3: cell 3; the dead local player halves on the 0x10 phase.
    z.def_type = 3;
    z.team = 1;
    b = death_map_zone_blip(z, facts, 0);
    CHECK(b.draw && b.color == 0xFF304080u && b.cell == 3);
    z.dead = true;
    z.local_player = true;
    CHECK(death_map_zone_blip(z, facts, 0x10).color == 0x7F182040u);
    CHECK(death_map_zone_blip(z, facts, 0x0F).color == 0xFF304080u);
    z.dead = false;
    z.local_player = false;
    // Type 2: nothing.
    z.def_type = 2;
    CHECK(!death_map_zone_blip(z, facts, 0).draw);
    // Attrib 0x20: orange cell 4, nothing on a vehicle parent or dead.
    z.def_type = 1;
    z.def_attrib = 0x20u;
    b = death_map_zone_blip(z, facts, 0);
    CHECK(b.draw && b.color == 0xFF907000u && b.cell == 4);
    z.parent_item = true;
    CHECK(!death_map_zone_blip(z, facts, 0).draw);
    z.parent_item = false;
    z.dead = true;
    CHECK(!death_map_zone_blip(z, facts, 0).draw);
    z.dead = false;
    // The team fallback: team 0 green, past team 2 grey.
    z.def_attrib = 0;
    z.team = 0;
    CHECK(death_map_zone_blip(z, facts, 0).color == 0xFF208020u);
    z.team = 4;
    CHECK(death_map_zone_blip(z, facts, 0).color == 0xFF707070u);
    // The capture-point pick: in session on 0x10004, a flag base draws size 6.
    facts.in_session = true;
    facts.game_type = 0x10004u;
    z.def_id = 4100;
    b = death_map_zone_blip(z, facts, 0);
    CHECK(b.color == 0xFF802020u && b.cell == 6);
    // A flag carried by the other team is not a capture point.
    z.def_id = 4091;
    z.occupant_present = true;
    z.occupant_team = 2;
    facts.hud_team = 1;
    CHECK(death_map_zone_blip(z, facts, 0).cell == 0);
    z.occupant_team = 1;
    b = death_map_zone_blip(z, facts, 0);
    CHECK(b.color == 0xFF304080u && b.cell == 2);
    // Attrib 0x20000: cell 0; a neutral zone being taken blinks the taker's
    // colour, an unnumbered owned zone blinks dark.
    facts.in_session = false;
    z.def_attrib = 0x20000u;
    z.team = 0;
    z.capture_team = 2;
    CHECK(death_map_zone_blip(z, facts, 0x20).color == 0xFF802020u);
    CHECK(death_map_zone_blip(z, facts, 0).color == 0xFF208020u);
    CHECK(death_map_zone_blip(z, facts, 0x20).cell == 0);
    z.capture_team = 0;
    z.team = 1;
    CHECK(death_map_zone_blip(z, facts, 0x20).color == 0x7F182040u);
    z.zone_number = 3;
    CHECK(death_map_zone_blip(z, facts, 0x20).color == 0xFF304080u);
}

void test_command_toggles_and_compile() {
    CommandMapView c;
    c.on_load();
    // GRID / TEXT / WAYPOINTS store the control's checked state;
    // CREATE_WAYPOINTS flips its byte whatever the control reports.
    c.toggle_changed(kCommandToggleGrid, false);
    CHECK(!c.toggle(kCommandToggleGrid));
    CHECK(command_map_mask(c.toggles) == 0xAE937u);
    c.toggle_changed(kCommandToggleGrid, true);
    c.toggle_changed(kCommandToggleCreateWaypoints, false);
    CHECK(c.toggle(kCommandToggleCreateWaypoints));
    c.toggle_changed(kCommandToggleCreateWaypoints, true);
    CHECK(!c.toggle(kCommandToggleCreateWaypoints));

    // The render pass: the rect clear first, the mode-4 map, the crosshair.
    HudMinimapInput in;
    in.surface_w = 1024.0f;
    in.surface_h = 768.0f;
    in.ticks = 8;
    DeathMapFacts facts;
    facts.player_present = true;
    facts.player_x = 50 * kWu;
    facts.player_y = 60 * kWu;
    facts.player_team = 2;
    MapViewRect r;
    r.left = 100;
    r.top = 50;
    r.right = 700;
    r.bottom = 550;
    // A placed waypoint on the player projects to the view centre.
    facts.user_waypoints[3].live = true;
    facts.user_waypoints[3].x = facts.player_x;
    facts.user_waypoints[3].y = facts.player_y;
    facts.user_waypoints[3].name = "HQ";
    CommandMapCompiler compiler;
    HudMapWindowPass out;
    CommandMapWaypointAnchor anchors[kCommandMapWaypointSlots];
    compiler.compile(in, c, r, 800, facts, out, anchors);
    CHECK(out.map.visible);
    CHECK(!anchors[0].live && anchors[3].live);
    CHECK(anchors[3].x == static_cast<int32_t>(out.map.center_x));
    CHECK(anchors[3].y == static_cast<int32_t>(out.map.center_y));
    CHECK(in.map_mode == 4);
    CHECK(in.flags == 0xAF937u);
    CHECK(out.map.clear.size() >= 2);
    if (out.map.clear.size() >= 2) {
        CHECK(out.map.clear[0].color == 0xFF000018u);
        CHECK(out.map.clear[0].a.x == 100.0f && out.map.clear[0].a.y == 50.0f);
        CHECK(out.map.clear[0].c.x == 700.0f && out.map.clear[0].c.y == 550.0f);
    }
    // The view centres on the player (no pan): the crosshair crosses the
    // rect centre, team 2 red at alpha 0x70.
    CHECK(out.over_lines.size() == 2);
    if (out.over_lines.size() == 2) {
        CHECK(out.over_lines[0].y0 == out.map.center_y);
        CHECK(out.over_lines[1].x0 == out.map.center_x);
        CHECK(out.over_lines[0].color == 0x70FF4040u);
    }
    // The zoom follows the view: a wheel step in shrinks the world per pixel.
    HudMinimapInput in2;
    in2.surface_w = 1024.0f;
    in2.surface_h = 768.0f;
    const float before = in.window_scale;
    c.on_event(MapViewEvent::kWheel, 0, 0, 0, 1);
    compiler.compile(in2, c, r, 800, facts, out, anchors);
    CHECK(in2.window_scale < before);
}

// The CMAP user-waypoint legs: the event routing, the name dialog's
// placement, the confirm's world point, the hover box and the delete button.
void test_command_waypoints() {
    using R = CommandMapView::EventResult;
    CommandMapView c;
    c.on_load();
    CHECK(c.on_event(MapViewEvent::kMove, 5, 5, 0, 0) == R::kHoverTest);
    CHECK(c.on_event(MapViewEvent::kLeftDown, 5, 5, 0, 0) == R::kNone);
    CHECK(c.on_event(MapViewEvent::kMove, 6, 5, kMapViewButtonLeft, 0) == R::kNone);
    c.on_event(MapViewEvent::kLeftUp, 6, 5, 0, 0);
    c.toggles.create_waypoints = true;
    CHECK(c.on_event(MapViewEvent::kLeftDown, 5, 5, 0, 0) == R::kPlaceWaypoint);

    // WAYPOINTNAME_DLG (0,0,200,50) against the MAP control's own rect.
    const MapViewRect dialog{0, 0, 200, 50};
    const MapViewRect map{0, 0, 730, 440};
    MapViewRect d = command_map_waypoint_dialog_rect(400, 200, dialog, map);
    CHECK(d.left == 300 && d.right == 500 && d.top == 175 && d.bottom == 225);
    d = command_map_waypoint_dialog_rect(10, 100, dialog, map);
    CHECK(d.left == 0 && d.right == 200 && d.top == 75 && d.bottom == 125);
    d = command_map_waypoint_dialog_rect(700, 300, dialog, map);
    CHECK(d.left == 530 && d.right == 730 && d.top == 275 && d.bottom == 325);

    // The confirm: S 1, an 800x600 window, zoom 1: P = 65536 / 160000 x
    // 65536; 100 design units off the centre is 2684354 world units.
    CommandMapView v;
    v.view.pixel_ratio = 1.0f;
    v.view.window_w = 800;
    v.view.window_h = 600;
    v.view.zoom = 1.0f;
    v.view.pan_x = 7;
    v.view.pan_y = -9;
    v.view.last_x = 300;
    v.view.last_y = 200;
    int32_t wx = 0, wy = 0;
    command_map_waypoint_world(v, 1000000, 2000000, wx, wy);
    CHECK(wx == 1000000 - 2684354 + 7);
    CHECK(wy == 2000000 + 2684354 - 9);
    v.view.last_x = 400;
    v.view.last_y = 300;
    command_map_waypoint_world(v, 1000000, 2000000, wx, wy);
    CHECK(wx == 1000007 && wy == 1999991);

    // The hover box: the device anchor (200, 100) at scale 2 is design
    // (100, 50); the box runs from 5 + the 12-wide button left of it to the
    // name's device width right, down the name's height.
    CommandMapWaypointAnchor anchors[kCommandMapWaypointSlots];
    anchors[0] = {true, 200, 100, 50, 10};
    anchors[2] = {true, 600, 400, 50, 10};
    bool hover[kCommandMapWaypointSlots] = {};
    hover[2] = true;
    command_map_waypoint_hover(anchors, hover, kCommandMapWaypointSlots, 90, 55, 2.0f, 2.0f, 12);
    CHECK(hover[0] && hover[2]); // the walk stops at the first hit
    command_map_waypoint_hover(anchors, hover, kCommandMapWaypointSlots, 83, 55, 2.0f, 2.0f, 12);
    CHECK(!hover[0] && !hover[2]); // strict edges; every slot cleared
    command_map_waypoint_hover(anchors, hover, kCommandMapWaypointSlots, 149, 59, 2.0f, 2.0f, 12);
    CHECK(hover[0]);
    int32_t bx = 0, by = 0;
    CHECK(command_map_close_button_position(anchors, hover, kCommandMapWaypointSlots, 2.0f, 2.0f,
            12, bx, by));
    CHECK(bx == 91 && by == 50); // (200 - 12 - 5) / 2, 100 / 2
    hover[0] = false;
    CHECK(!command_map_close_button_position(anchors, hover, kCommandMapWaypointSlots, 2.0f,
            2.0f, 12, bx, by));
}

} // namespace

int main() {
    test_integer_pow();
    test_design_to_device();
    test_death_load();
    test_death_render_and_pan();
    test_death_zoom();
    test_death_fit_and_ease();
    test_command_map();
    test_death_compile();
    test_zone_blip_pick();
    test_command_toggles_and_compile();
    test_command_waypoints();
    if (failures == 0) std::printf("hud_map_view: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
