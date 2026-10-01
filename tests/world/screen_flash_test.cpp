// The three fullscreen damage-feedback words: their arms, the four-word decay
// run they share with the camera shake, the revive tint's 0xC4 hold, the
// explosive white FLOOR, the joiner health-drop gate, and the draw reductions
// (the red cap + camera-mode suppression, the revive tint channel, the HUD
// early return).
// [orig: Player_OnDamageReceived @0x4DD880; Entity_OnDamageReceived @0x4AF800;
//  Entity_ApplyCollisionForce @0x4AF4A0; NapiNPClientMsg_0x00A @0x43059a;
//  NapiNPClientMsg_0x03A @0x422685; Player_UpdatePerFrame @0x4DE5A7..0x4DE5F7;
//  Game_InitNewRound @0x422778/@0x422784/@0x422790;
//  Render_ProcessMainSceneFrame @0x5CAB9A..0x5CAC48;
//  HUD_RenderAllOverlays @0x5a8098]
#include <runtime/world/local_player.h>
#include <runtime/world/player_view.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <memory>

using namespace opennova::world;

#define CHECK(c)                                                                    \
    do {                                                                            \
        if (!(c)) {                                                                 \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);                        \
            return 1;                                                               \
        }                                                                           \
    } while (0)

// The red arm saturates at 255 and never wraps; three damage events reach the
// ceiling. [orig: @0x4dd88f..0x4dd896]
static int test_red_arm_caps() {
    ScreenFlashState f;
    screen_flash_add_red(f, kScreenFlashRedArm);
    CHECK(f.red == 120);
    screen_flash_add_red(f, kScreenFlashRedArm);
    CHECK(f.red == 240);
    screen_flash_add_red(f, kScreenFlashRedArm);
    CHECK(f.red == 255);
    screen_flash_add_red(f, kScreenFlashRedArm);
    CHECK(f.red == 255);
    return 0;
}

// The white word: the collision blackout HARD-SETS 255, the explosive ammo leg
// only raises a FLOOR of 128 and never lowers a burning flash.
// [orig: @0x4af729 / @0x4af769; @0x4af828..0x4af82a]
static int test_white_arms() {
    ScreenFlashState f;
    screen_flash_arm_white_explosive(f);
    CHECK(f.white == 128);
    f.white = 200;
    screen_flash_arm_white_explosive(f);
    CHECK(f.white == 200); // a floor, not a set
    f.white = 64;
    screen_flash_arm_white_explosive(f);
    CHECK(f.white == 128);
    screen_flash_arm_white_hit(f);
    CHECK(f.white == 255);
    screen_flash_arm_white_explosive(f);
    CHECK(f.white == 255);
    return 0;
}

// The decay run, in retail's instruction order: white -4 with a <= 3 snap, red
// -2 with a <= 1 snap, revive -1 with a 0xC4 floor and NO else branch.
// [orig: @0x4DE5A7..0x4DE5F7]
static int test_decays() {
    ScreenFlashState f;
    // WHITE: 255 -> 0 in 64 ticks (63 subtractions of 4 land on 3, which snaps).
    screen_flash_arm_white_hit(f);
    int ticks = 0;
    while (f.white != 0 && ticks < 1000) {
        screen_flash_decay(f);
        ++ticks;
    }
    CHECK(ticks == 64);
    // The snap: anything at or below 3 goes straight to 0.
    for (int start = 0; start <= 3; ++start) {
        ScreenFlashState w;
        w.white = start;
        screen_flash_decay(w);
        CHECK(w.white == 0);
    }
    ScreenFlashState w4;
    w4.white = 4;
    screen_flash_decay(w4);
    CHECK(w4.white == 0); // 4 > 3 -> 4 - 4 = 0
    ScreenFlashState w5;
    w5.white = 5;
    screen_flash_decay(w5);
    CHECK(w5.white == 1);

    // RED: 255 -> 0 in 128 ticks, with the <= 1 snap catching the odd tail.
    ScreenFlashState r;
    screen_flash_add_red(r, 255);
    ticks = 0;
    while (r.red != 0 && ticks < 1000) {
        screen_flash_decay(r);
        ++ticks;
    }
    CHECK(ticks == 128);
    ScreenFlashState r1;
    r1.red = 1;
    screen_flash_decay(r1);
    CHECK(r1.red == 0);
    ScreenFlashState r2;
    r2.red = 2;
    screen_flash_decay(r2);
    CHECK(r2.red == 0);

    // REVIVE: 255 slides to 196 over 59 ticks, then HOLDS there forever.
    ScreenFlashState v;
    screen_flash_arm_revive(v);
    CHECK(v.revive == 255);
    for (int i = 0; i < 59; ++i) screen_flash_decay(v);
    CHECK(v.revive == kScreenFlashReviveFloor);
    CHECK(v.revive == 196);
    for (int i = 0; i < 500; ++i) screen_flash_decay(v);
    CHECK(v.revive == kScreenFlashReviveFloor);
    // The missing else: a word already at 0 or 1 is left exactly where it is.
    ScreenFlashState v0;
    screen_flash_decay(v0);
    CHECK(v0.revive == 0);
    ScreenFlashState v1;
    v1.revive = 1;
    screen_flash_decay(v1);
    CHECK(v1.revive == 1);

    // The three decay independently in one call.
    ScreenFlashState all;
    all.white = 100;
    all.red = 100;
    all.revive = 255;
    screen_flash_decay(all);
    CHECK(all.white == 96 && all.red == 98 && all.revive == 254);
    return 0;
}

// Only the round clear puts the held revive tint out.
// [orig: Game_InitNewRound @0x422778 / @0x422784 / @0x422790]
static int test_round_clear() {
    ScreenFlashState f;
    screen_flash_arm_white_hit(f);
    screen_flash_add_red(f, kScreenFlashRedArm);
    screen_flash_track_revive(f, true);
    CHECK(f.white != 0 && f.red != 0 && f.revive != 0 && f.revive_latched);
    screen_flash_clear(f);
    CHECK(f.white == 0 && f.red == 0 && f.revive == 0 && !f.revive_latched);
    return 0;
}

// The revive arm rides the being-revived latch's RISING edge, once per revive.
// [orig: NapiNPClientMsg_0x03A @0x422685]
static int test_revive_edge() {
    ScreenFlashState f;
    screen_flash_track_revive(f, false);
    CHECK(f.revive == 0);
    screen_flash_track_revive(f, true);
    CHECK(f.revive == 255);
    // Held: the tint decays to its floor without being re-armed.
    for (int i = 0; i < 59; ++i) {
        screen_flash_track_revive(f, true);
        screen_flash_decay(f);
    }
    CHECK(f.revive == kScreenFlashReviveFloor);
    // Dropping and re-raising the latch arms again.
    screen_flash_track_revive(f, false);
    CHECK(f.revive == kScreenFlashReviveFloor); // the drop does not clear the word
    screen_flash_track_revive(f, true);
    CHECK(f.revive == 255);
    return 0;
}

// The joiner's 0x0A tail arms ONLY on a signed decrease, and never on a hold or
// a heal. [orig: @0x43059a `cmp dx,[eax+0x11E]` / `jge` @0x4305a1]
static int test_health_drop_gate() {
    ScreenFlashState f;
    CameraShakeState s;
    screen_flash_arm_health_drop(f, s, 100, 100); // unchanged
    CHECK(f.red == 0 && s.counter == 0);
    screen_flash_arm_health_drop(f, s, 100, 60); // healed
    CHECK(f.red == 0 && s.counter == 0);
    screen_flash_arm_health_drop(f, s, 60, 100); // dropped
    CHECK(f.red == kScreenFlashRedArm);
    CHECK(s.counter == kShakeArmHealthDrop);
    screen_flash_arm_health_drop(f, s, 0, 60); // dropped to death
    CHECK(f.red == 2 * kScreenFlashRedArm);
    CHECK(s.counter == 2 * kShakeArmHealthDrop);
    // Both terms saturate at 255.
    for (int i = 0; i < 40; ++i) screen_flash_arm_health_drop(f, s, -i - 1, -i);
    CHECK(f.red == 255 && s.counter == 255);
    return 0;
}

// The draw reductions [orig: Render_ProcessMainSceneFrame @0x5CAB9A..0x5CAC48].
static int test_draw_values() {
    ScreenFlashState f;
    // The red quad's alpha is capped at 0xC0 and suppressed in camera mode 3.
    f.red = 100;
    CHECK(screen_flash_red_draw_alpha(f, 0) == 100);
    f.red = 255;
    CHECK(screen_flash_red_draw_alpha(f, 0) == kScreenFlashRedDrawCap);
    CHECK(screen_flash_red_draw_alpha(f, 1) == kScreenFlashRedDrawCap);
    CHECK(screen_flash_red_draw_alpha(f, 4) == kScreenFlashRedDrawCap);
    CHECK(screen_flash_red_draw_alpha(f, 3) == 0); // the free/spectator camera
    f.red = 0;
    CHECK(screen_flash_red_draw_alpha(f, 0) == 0);
    // The revive tint: R = G = 255 - (word >> 1), B and A stay 255.
    f.revive = 0;
    CHECK(screen_flash_revive_channel(f) == 255);
    f.revive = 255;
    CHECK(screen_flash_revive_channel(f) == 255 - 127);
    f.revive = kScreenFlashReviveFloor;
    CHECK(screen_flash_revive_channel(f) == 255 - 98);
    // The HUD overlay pass early-returns for as long as the white word burns.
    ScreenFlashState h;
    CHECK(!screen_flash_hud_overlays_suppressed(h));
    screen_flash_arm_white_hit(h);
    CHECK(screen_flash_hud_overlays_suppressed(h));
    int suppressed_ticks = 0;
    while (screen_flash_hud_overlays_suppressed(h) && suppressed_ticks < 1000) {
        screen_flash_decay(h);
        ++suppressed_ticks;
    }
    CHECK(suppressed_ticks == 64); // the full white flash blanks the HUD 64 ticks
    return 0;
}

// Player_OnDamageReceived through a bound local player: red 120, shake 10, both
// capped; a world with no local player state is a no-op.
// [orig: Player_OnDamageReceived @0x4DD880]
static int test_player_on_damage_received() {
    auto heap = std::make_unique<World>();
    World &world = *heap;
    const int32_t at[3] = {0, 0, 0};
    const RadarSource none;
    player_on_damage_received(world, none, at); // no local player bound: must not fault
    auto local = std::make_unique<LocalPlayer>(world);
    world.local_player_state = local.get();
    player_on_damage_received(world, none, at);
    CHECK(local->view.flash.red == kScreenFlashRedArm);
    CHECK(local->view.shake.counter == kShakeArmDamageReceived);
    player_on_damage_received(world, none, at);
    CHECK(local->view.flash.red == 2 * kScreenFlashRedArm);
    CHECK(local->view.shake.counter == 2 * kShakeArmDamageReceived);
    for (int i = 0; i < 40; ++i) player_on_damage_received(world, none, at);
    CHECK(local->view.flash.red == 255);
    CHECK(local->view.shake.counter == 255);
    // The white word is NOT this function's; only the collision/explosive legs
    // write it.
    CHECK(local->view.flash.white == 0);
    world.local_player_state = nullptr;
    return 0;
}

int main() {
    if (test_red_arm_caps() != 0) return 1;
    if (test_white_arms() != 0) return 1;
    if (test_decays() != 0) return 1;
    if (test_round_clear() != 0) return 1;
    if (test_revive_edge() != 0) return 1;
    if (test_health_drop_gate() != 0) return 1;
    if (test_draw_values() != 0) return 1;
    if (test_player_on_damage_received() != 0) return 1;
    std::printf("screen_flash: OK\n");
    return 0;
}
