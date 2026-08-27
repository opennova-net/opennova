// The footstep slot pick's witnessed test ORDER [orig: org2 @0x4b77c6-0x4b78a8;
// sibling org1 @0x4bf23e-0x4bf2b0]. Order is the whole content of this
// function: water beats on-entity beats snow beats ground, and the first match
// wins. One implementation serves both body channels (the authority/AI bodies
// and the wire-fed remote bodies), so a regression here changes both at once.
#include <cstdio>
#include <string>

#include <runtime/audio/footstep_slot.h>

using namespace opennova::audio;

static int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

constexpr int32_t kUnit = 65536;

void test_ground_is_the_default() {
    CHECK(footstep_slot(/*feet_z=*/0, /*water_z=*/0, false, /*surface=*/0, 0) ==
          kSlotFootLGround);
    CHECK(footstep_slot(0, 0, false, 0, 1) == kSlotFootRGround);
}

// Surface type 3 is snow [orig: the ==3 test @0x4b77c6 block].
void test_snow_is_surface_type_three_only() {
    CHECK(footstep_slot(0, 0, false, 3, 0) == kSlotFootLSnow);
    CHECK(footstep_slot(0, 0, false, 3, 1) == kSlotFootRSnow);
    // Any other surface stays ground — 2 and 4 must not be snow.
    CHECK(footstep_slot(0, 0, false, 2, 0) == kSlotFootLGround);
    CHECK(footstep_slot(0, 0, false, 4, 0) == kSlotFootLGround);
}

// Standing on another entity beats the surface test.
void test_on_entity_beats_snow() {
    CHECK(footstep_slot(0, 0, /*on_entity=*/true, /*surface=*/3, 0) == kSlotFootLObject);
    CHECK(footstep_slot(0, 0, true, 3, 1) == kSlotFootRObject);
}

// Water beats everything, and uses ONE slot for both feet.
void test_water_beats_everything_and_ignores_foot() {
    const int32_t water = 10 * kUnit;
    CHECK(footstep_slot(/*feet_z=*/5 * kUnit, water, true, 3, 0) == kSlotFootWater);
    CHECK(footstep_slot(5 * kUnit, water, true, 3, 1) == kSlotFootWater);
    // Feet AT or ABOVE the plane are not in the water (strict <).
    CHECK(footstep_slot(water, water, false, 0, 0) == kSlotFootLGround);
    CHECK(footstep_slot(11 * kUnit, water, false, 0, 0) == kSlotFootLGround);
}

// water_z == 0 means "no water plane" — a body below zero must not splash.
void test_zero_water_plane_is_disabled_not_sea_level() {
    CHECK(footstep_slot(/*feet_z=*/-5 * kUnit, /*water_z=*/0, false, 0, 0) ==
          kSlotFootLGround);
}

// organic_slot_set — the wire body channel's profile resolve, mirroring the
// authority path's fallback chain [orig: Entity_GetProfileSlotSound @0x528300;
// the "default" seed @0x49e3f5; the find-miss base @0x526e30].
constexpr char kProfiles[] =
    "begin \"default\"\n"
    "     SSLFootGND     FSP_DIRT_L\n"
    "end\n"
    "begin \"SP_Man\"\n"
    "     SSLFootGND     FSP_MAN_L\n"
    "     ssaudio1       RUSTLE_M\n"
    "end\n"
    "begin \"SP_Woman\"\n"
    "     SSLFootGND     FSP_WOMAN_L\n"
    "end\n";

void test_organic_slot_set_resolve_chain() {
    SoundProfileTable profiles;
    CHECK(profiles.parse(kProfiles, sizeof(kProfiles) - 1) == 3);
    OrganicSoundProfileTable bindings;
    OrganicSoundProfile bound;
    bound.primary = 1; // SP_Man
    bound.female = 2;  // SP_Woman
    bindings.set(42, bound);

    // The bound primary resolves its authored set.
    const std::string *set =
        organic_slot_set(profiles, bindings, 42, /*female=*/false, kSlotFootLGround);
    CHECK(set != nullptr && *set == "FSP_MAN_L");
    // The female byte selects the female binding.
    set = organic_slot_set(profiles, bindings, 42, true, kSlotFootLGround);
    CHECK(set != nullptr && *set == "FSP_WOMAN_L");
    // An UNRESOLVED female binding falls to "default", never back to the
    // primary — the authority path's unconditional select.
    OrganicSoundProfile detached;
    detached.primary = 1;
    detached.female = -1;
    bindings.set(43, detached);
    set = organic_slot_set(profiles, bindings, 43, true, kSlotFootLGround);
    CHECK(set != nullptr && *set == "FSP_DIRT_L");
    // An unbound item type resolves through "default".
    set = organic_slot_set(profiles, bindings, 99, false, kSlotFootLGround);
    CHECK(set != nullptr && *set == "FSP_DIRT_L");
    // An empty authored slot is the resolved-id-0 no-op: play NOTHING.
    set = organic_slot_set(profiles, bindings, 99, false, kSlotAudio1);
    CHECK(set == nullptr);
    // A bound profile with the slot authored still resolves it.
    set = organic_slot_set(profiles, bindings, 42, false, kSlotAudio1);
    CHECK(set != nullptr && *set == "RUSTLE_M");
    // Out-of-range slots and an empty profile table resolve to nothing.
    CHECK(organic_slot_set(profiles, bindings, 42, false, -1) == nullptr);
    CHECK(organic_slot_set(profiles, bindings, 42, false, 1000) == nullptr);
    SoundProfileTable empty;
    CHECK(organic_slot_set(empty, bindings, 42, false, kSlotFootLGround) == nullptr);
}

} // namespace

int main() {
    test_ground_is_the_default();
    test_snow_is_surface_type_three_only();
    test_on_entity_beats_snow();
    test_water_beats_everything_and_ignores_foot();
    test_zero_water_plane_is_disabled_not_sea_level();
    test_organic_slot_set_resolve_chain();
    if (failures == 0) std::printf("footstep_slot_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
