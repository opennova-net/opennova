// world::wire_body_slot_sounds — the wire-fed remote body's footstep/foley
// consume, pinned headless: foley precedes the feet inside one trigger word
// [orig: org1 foley @0x4bf169-0x4bf23e then feet @0x4bf23e-0x4bf2b0; org2
// foley @0x4b76f1-0x4b77c6 then feet @0x4b77c6-0x4b78a8], foley at the body
// ORIGIN with no dip, footsteps dipped to FOOT level [orig: the dip
// @0x4b77d3] through the shared slot pick, the player-only female profile
// select, and the resolved-id-0 no-op.
#include <cstdio>
#include <cstring>
#include <string>

#include <runtime/world/wire_body_sound.h>
#include <runtime/world/world.h>

using namespace opennova;
using namespace opennova::world;

static int failures = 0;
#define CHECK(c) \
    do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

namespace {

constexpr int32_t kUnit = 65536;

constexpr char kProfiles[] =
    "begin \"default\"\n"
    "     SSLFootGND     FSP_DIRT_L\n"
    "     SSRFootGND     FSP_DIRT_R\n"
    "end\n"
    "begin \"SP_Man\"\n"
    "     SSLFootGND     FSP_MAN_L\n"
    "     SSRFootGND     FSP_MAN_R\n"
    "     SSLFootOBJ     FSP_MAN_WOOD_L\n"
    "     SSFootWater    FS_WATER\n"
    "     ssaudio1       RUSTLE_M\n"
    "     ssaudio3       GEAR_M\n"
    "end\n"
    "begin \"SP_Woman\"\n"
    "     SSLFootGND     FSP_WOMAN_L\n"
    "end\n";

void seed_world(World &w) {
    CHECK(w.tables.sound_profiles.parse(kProfiles, sizeof(kProfiles) - 1) == 3);
    audio::OrganicSoundProfile op;
    op.primary = 1; // SP_Man
    op.female = 2;  // SP_Woman
    w.tables.organic_sound_profiles.set(42, op);
}

// Foley fires before the feet within ONE word, foley sits at the body origin,
// and the footstep is dipped by the capsule bottom.
void test_foley_precedes_feet_and_positions() {
    World w;
    seed_world(w);
    const int32_t body[3] = {10 * kUnit, 20 * kUnit, 5 * kUnit};
    // Word: SSAudio1 (0x20) + SSAudio3 (0x80) + LEFT foot (0x1).
    const uint32_t words[1] = {0x20u | 0x80u | 0x1u};
    wire_body_slot_sounds(w, words, 1, /*capsule_bottom=*/2 * kUnit,
                          /*item_id=*/42, /*character_id=*/0,
                          /*source_handle=*/7, /*on_entity=*/false, body);
    CHECK(w.out.slot_sounds.size() == 3);
    if (w.out.slot_sounds.size() == 3) {
        CHECK(std::strcmp(w.out.slot_sounds[0].set_name, "RUSTLE_M") == 0);
        CHECK(std::strcmp(w.out.slot_sounds[1].set_name, "GEAR_M") == 0);
        CHECK(std::strcmp(w.out.slot_sounds[2].set_name, "FSP_MAN_L") == 0);
        // Foley at the ORIGIN, no dip.
        CHECK(w.out.slot_sounds[0].pos[2] == 5 * kUnit);
        // The footstep dips by the frame's capsule bottom (origin - bottom).
        CHECK(w.out.slot_sounds[2].pos[2] == 3 * kUnit);
        CHECK(w.out.slot_sounds[2].pos[0] == 10 * kUnit);
        CHECK(w.out.slot_sounds[0].source_handle == 7);
        CHECK(w.out.slot_sounds[2].slot == audio::kSlotFootLGround);
    }
}

// The wire carrier link stands in for Entity::ground_target — the on-entity
// slot wins over ground; the water plane beats both.
void test_slot_pick_reads_carrier_and_water() {
    World w;
    seed_world(w);
    const int32_t body[3] = {0, 0, 5 * kUnit};
    const uint32_t words[1] = {0x1u};
    wire_body_slot_sounds(w, words, 1, 0, 42, 0, 7, /*on_entity=*/true, body);
    CHECK(w.out.slot_sounds.size() == 1);
    if (!w.out.slot_sounds.empty()) {
        CHECK(w.out.slot_sounds[0].slot == audio::kSlotFootLObject);
        CHECK(std::strcmp(w.out.slot_sounds[0].set_name, "FSP_MAN_WOOD_L") == 0);
    }
    w.out.slot_sounds.clear();
    w.env.water_z = 10 * kUnit; // feet below the plane -> the one water slot
    wire_body_slot_sounds(w, words, 1, 0, 42, 0, 7, true, body);
    CHECK(w.out.slot_sounds.size() == 1);
    if (!w.out.slot_sounds.empty())
        CHECK(w.out.slot_sounds[0].slot == audio::kSlotFootWater);
}

// A player row's packed avatar id selects the female profile; an NPC row
// (character_id 0) keeps the primary even when id 0 is marked female.
void test_female_select_is_player_only() {
    World w;
    seed_world(w);
    w.tables.character_traits.set(9, true);
    w.tables.character_traits.set(0, true);
    const int32_t body[3] = {0, 0, 0};
    const uint32_t words[1] = {0x1u};
    wire_body_slot_sounds(w, words, 1, 0, 42, /*character_id=*/9, 7, false, body);
    CHECK(w.out.slot_sounds.size() == 1);
    if (!w.out.slot_sounds.empty())
        CHECK(std::strcmp(w.out.slot_sounds[0].set_name, "FSP_WOMAN_L") == 0);
    w.out.slot_sounds.clear();
    wire_body_slot_sounds(w, words, 1, 0, 42, /*character_id=*/0, 7, false, body);
    CHECK(w.out.slot_sounds.size() == 1);
    if (!w.out.slot_sounds.empty())
        CHECK(std::strcmp(w.out.slot_sounds[0].set_name, "FSP_MAN_L") == 0);
}

// An unauthored slot is the resolved-id-0 no-op: NOTHING plays, no fallback.
// SP_Woman authors no right-ground set, and SP_Man authors no ssaudio2.
void test_empty_set_plays_nothing() {
    World w;
    seed_world(w);
    w.tables.character_traits.set(9, true);
    const int32_t body[3] = {0, 0, 0};
    const uint32_t words[1] = {0x2u | 0x40u}; // RIGHT foot + SSAudio2
    wire_body_slot_sounds(w, words, 1, 0, 42, 9, 7, false, body);
    CHECK(w.out.slot_sounds.empty());
}

// Words consume in playhead order — two crossed frames fire both, in order.
void test_multi_word_order() {
    World w;
    seed_world(w);
    const int32_t body[3] = {0, 0, 0};
    const uint32_t words[3] = {0x1u, 0u, 0x2u};
    wire_body_slot_sounds(w, words, 3, 0, 42, 0, 7, false, body);
    CHECK(w.out.slot_sounds.size() == 2);
    if (w.out.slot_sounds.size() == 2) {
        CHECK(w.out.slot_sounds[0].slot == audio::kSlotFootLGround);
        CHECK(w.out.slot_sounds[1].slot == audio::kSlotFootRGround);
    }
}

void test_degenerate_inputs() {
    World w;
    seed_world(w);
    const int32_t body[3] = {0, 0, 0};
    wire_body_slot_sounds(w, nullptr, 4, 0, 42, 0, 7, false, body);
    const uint32_t words[1] = {0x1u};
    wire_body_slot_sounds(w, words, 0, 0, 42, 0, 7, false, body);
    CHECK(w.out.slot_sounds.empty());
}

} // namespace

int main() {
    test_foley_precedes_feet_and_positions();
    test_slot_pick_reads_carrier_and_water();
    test_female_select_is_player_only();
    test_empty_set_plays_nothing();
    test_multi_word_order();
    test_degenerate_inputs();
    if (failures == 0) std::printf("wire_body_sound_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
