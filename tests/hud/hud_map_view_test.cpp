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
    // Three walked blips plus each zone's redraw.
    CHECK(out.map.sprites.size() == 6);
    // Zone A: the wave zone's colour, the letter at the anchor less half the
    // 8x16 cell, and its queued/countdown pair one cell below (no hold).
    CHECK(has_label(out.map, "A", 291.0f, 252.0f, 0xFFC8C814u));
    CHECK(has_label(out.map, "2/17", 291.0f, 268.0f, 0xFFC8C814u));
    // Zone B: the other team brightens 20/32 and has no score lines.
    const float bx = static_cast<float>(static_cast<int>(295.0f +
            20.0f / frame.scale - 4.0f));
    CHECK(has_label(out.map, "B", bx, 252.0f, 0xFFCFABABu));
    // Zone C: timer not ready on a non-0x50010 game type -> nothing.
    for (const HudMapLabel &l : out.map.labels) CHECK(std::strcmp(l.text, "C") != 0);
    CHECK(out.map.labels.size() == 3);
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
    CHECK(has_label(out.map, "9", 291.0f, 236.0f, 0xFFC8C814u));
    CHECK(has_label(out.map, "B", bx, 252.0f, 0xFF802020u));
    // Game type 0x50010 letters every zone.
    facts.game_type = 0x50010;
    compiler.compile(in, frame, facts, out);
    bool has_c = false;
    for (const HudMapLabel &l : out.map.labels) has_c = has_c || std::strcmp(l.text, "C") == 0;
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
    for (const HudMapLabel &l : out.map.labels) CHECK(std::strcmp(l.text, "A") != 0);
    // The pulse: an own-team zone at phase 16 moves half-way to white.
    in.markers[0].flags = 0;
    facts.self_zone_handle = 0xFFFF;
    in.ticks = 8 + 16;
    compiler.compile(in, frame, facts, out);
    CHECK(has_label(out.map, "A", 291.0f, 252.0f, 0xFF979FBFu));
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
    CommandMapCompiler compiler;
    HudMapWindowPass out;
    compiler.compile(in, c, r, 800, facts, out);
    CHECK(out.map.visible);
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
    compiler.compile(in2, c, r, 800, facts, out);
    CHECK(in2.window_scale < before);
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
    test_command_toggles_and_compile();
    if (failures == 0) std::printf("hud_map_view: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
