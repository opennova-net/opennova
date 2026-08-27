// charattr.def ATTRIBUTES -> World::class_attribute_flags -> the friendly-tag
// medic plate (and the map medic marker that reads the same bit): the whole
// per-class word, the first-missing-section stop, the first-section-wins
// duplicate rule, the 16-slot class wrap, and the S2C 0x41 clear.
// [orig: CharAttr_LoadFromDef @0x412140 (the per-class ATTRIBUTES word at
//  g_CharAttr row +0x28, names @0x813F18); AnimMap_IsSlotActive @0x4125e0 (the
//  reader); AnimMap_SetSlotProperty @0x412890 (the 0x41 clear)]
#include <runtime/world/entity.h>
#include <runtime/world/friendly_tags.h>
#include <runtime/world/world.h>

#include <net/npruntime/charattr_challenge.h>

#include <cstdio>
#include <cstring>
#include <vector>

using namespace opennova::world;

static int failures = 0;
#define CHECK(c) \
    do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

namespace {

// Retail-shaped text: the JO:CA Charattr.def layout -- the comment banner, INI
// sections, tab-padded `KEY = value` rows, a NULL attribute, a two-token line,
// a DUPLICATE section later in the file (the first wins, like
// ConfigFile_FindSection), and a gap before CHARACTER7 (the sequential walk
// stops at the first missing section, so 7 never lives).
const char kFixture[] =
    "//////////////////////////////////////////////////////////////////////\n"
    "//\n"
    "// Character Def File\n"
    "//\n"
    "// Attributes: AutoScope KnifeBonus Medic  WaterGirl\n"
    "//////////////////////////////////////////////////////////////////////\n"
    "\n"
    "[CHARACTER1]\n"
    "STEALTH\t\t\t= 25\n"
    "HPBONUS\t\t\t= 2\t\n"
    "RECOIL_MUTE \t= 0.75\t\n"
    "JUNGLE_CAMMO\t= 5310\n"
    "RUN_MODIFIER\t= 0\n"
    "ATTRIBUTES  \t= AutoScope \n"
    "\n"
    "[CHARACTER5]\n"
    "STEALTH\t\t\t= 25\n"
    "JUNGLE_CAMMO\t= 5305\n"
    "RUN_MODIFIER\t= 1\n"
    "ATTRIBUTES  \t= Medic \n"
    "\n"
    "[CHARACTER2]\n"
    "ATTRIBUTES  \t= NULL\n"
    "[CHARACTER3]\n"
    "ATTRIBUTES  \t= KnifeBonus\n"
    "[CHARACTER4]\n"
    "ATTRIBUTES  \t= Medic WaterGirl\n"
    "[CHARACTER5]\n"
    "ATTRIBUTES\t\t= NULL\n"
    "[CHARACTER7]\n"
    "ATTRIBUTES  \t= Medic\n";

EntityHandle spawn_organic(World &w, uint8_t team, uint8_t player_class,
                           bool player) {
    Entity e;
    e.kind = EntityKind::Organic;
    e.team = team;
    e.player_class = player_class;
    e.alive = true;
    e.has_item_def = true;
    if (player) e.flags |= kEntityFlagPlayer;
    return w.registry.spawn(0, e);
}

} // namespace

int main() {
    opennova::np::CharAttrChallengeTable table;
    CHECK(opennova::np::parse_charattr_challenge_table(
            reinterpret_cast<const uint8_t *>(kFixture), sizeof(kFixture) - 1,
            table));
    const auto rows = opennova::np::charattr_class_attribute_rows(table);
    // The WHOLE word per class, index = class - 1.
    CHECK(rows[0] == 0x1u);  // CHARACTER1: AutoScope
    CHECK(rows[1] == 0x0u);  // NULL names no attribute
    CHECK(rows[2] == 0x4u);  // KnifeBonus
    CHECK(rows[3] == 0x28u); // Medic | WaterGirl (0x10 is the table's unused slot)
    CHECK(rows[4] == 0x8u);  // the FIRST [CHARACTER5] wins over the later NULL one
    CHECK(rows[5] == 0x0u);  // CHARACTER6 is absent ...
    CHECK(rows[6] == 0x0u);  // ... so CHARACTER7 never lives (first-missing stop)
    for (size_t i = 7; i < rows.size(); ++i) CHECK(rows[i] == 0u);

    World w;
    w.registry.configure_pool(0, 16);
    w.class_attribute_flags = rows;
    CHECK(w.class_has_attribute(5, World::kCharAttrMedic));
    CHECK(w.class_has_attribute(4, World::kCharAttrMedic));
    CHECK(!w.class_has_attribute(1, World::kCharAttrMedic));
    CHECK(w.class_has_attribute(1, 0x1u));
    CHECK(!w.class_has_attribute(3, World::kCharAttrMedic));
    // Class 0 wraps to row 15 (empty); class 21 wraps to row 4 -- retail's
    // `(class - 1) & 0xF` select, kept as is.
    CHECK(!w.class_has_attribute(0, World::kCharAttrMedic));
    CHECK(w.class_has_attribute(21, World::kCharAttrMedic));

    // The friendly-tag feed reads the bit per entity class.
    const EntityHandle local = spawn_organic(w, 1, 1, true);
    const EntityHandle medic = spawn_organic(w, 1, 5, false);
    const EntityHandle rifle = spawn_organic(w, 1, 1, false);
    // The pass gate `g_GameType || death screen` [orig: @0x5a44e8].
    FriendlyTagPassContext ctx;
    ctx.game_type = 0x30020u;
    std::vector<FriendlyTagSource> tags;
    collect_friendly_tags(w, *w.registry.get(local), tags, ctx);
    CHECK(tags.size() == 2);
    bool saw_medic = false, saw_rifle = false;
    for (const FriendlyTagSource &t : tags) {
        if (t.entity == medic) { saw_medic = true; CHECK(t.medic); }
        if (t.entity == rifle) { saw_rifle = true; CHECK(!t.medic); }
    }
    CHECK(saw_medic && saw_rifle);

    // S2C 0x41 property 0 blanks every row's word: the plates go with it.
    opennova::np::clear_charattr_challenge_property(table, 0);
    w.class_attribute_flags = opennova::np::charattr_class_attribute_rows(table);
    CHECK(!w.class_has_attribute(5, World::kCharAttrMedic));
    CHECK(!w.class_has_attribute(1, 0x1u));
    tags.clear();
    collect_friendly_tags(w, *w.registry.get(local), tags, ctx);
    CHECK(tags.size() == 2);
    for (const FriendlyTagSource &t : tags) CHECK(!t.medic);

    // A property clear that is NOT id 0 leaves the words alone.
    opennova::np::CharAttrChallengeTable again;
    CHECK(opennova::np::parse_charattr_challenge_table(
            reinterpret_cast<const uint8_t *>(kFixture), sizeof(kFixture) - 1,
            again));
    opennova::np::clear_charattr_challenge_property(again, 3); // HPBONUS
    CHECK(opennova::np::charattr_class_attribute_rows(again)[4] == 0x8u);

    // An unloaded table (a missing charattr.def) reads as no attribute at all.
    w.class_attribute_flags = opennova::np::charattr_class_attribute_rows(
            opennova::np::CharAttrChallengeTable{});
    for (uint8_t c = 0; c < 32; ++c) CHECK(!w.class_has_attribute(c, 0xFFFFFFFFu));

    if (failures == 0) std::printf("charattr_flags_test: ok\n");
    return failures == 0 ? 0 : 1;
}
