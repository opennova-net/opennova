// The friendly-tag radio-request feeds (D-HUD-20 residue d): the per-tag
// fold of the S2C 0x6D latch (entity+885) with the Entity_FindChildByDefType
// carrier walk, and the local player's viewer gate (+0x168 in {2,5} or its
// own +885) that HudFrameState::radio_request_icon_viewer carries.
// [orig: HUD_DrawEntityLabel @0x5a3bba..0x5a3c1a; Entity_FindChildByDefType
//  @0x43bea0]
#include <runtime/world/entity.h>
#include <runtime/world/friendly_tags.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <vector>

using namespace opennova::world;

static int failures = 0;
#define CHECK(c) \
    do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

namespace {

EntityHandle spawn_organic(World &w, bool player, uint8_t radio_request) {
    Entity e;
    e.kind = EntityKind::Organic;
    e.team = 1;
    e.alive = true;
    e.has_item_def = true;
    e.radio_request = radio_request;
    if (player) e.flags |= kEntityFlagPlayer;
    return w.registry.spawn(0, e);
}

// A carried-on link: a def-bearing item of the given ItemDef+0x5C type
// (1 = vehicle), or a def-less row when `has_def` is false.
EntityHandle spawn_link(World &w, uint8_t item_type, bool has_def = true) {
    Entity e;
    e.kind = EntityKind::Item;
    e.has_item_def = has_def;
    e.item_type = item_type;
    return w.registry.spawn(1, e);
}

bool tag_radio_request(World &w, EntityHandle local, EntityHandle tagged) {
    FriendlyTagPassContext ctx;
    ctx.game_type = 0x30020u; // the pass gate `g_GameType || death screen`
    std::vector<FriendlyTagSource> tags;
    collect_friendly_tags(w, *w.registry.get(local), tags, ctx);
    CHECK(tags.size() == 1 && tags[0].entity == tagged);
    return !tags.empty() && tags[0].radio_request;
}

void test_tag_fold() {
    World w;
    w.registry.configure_pool(0, 8);
    w.registry.configure_pool(1, 32);
    const EntityHandle local = spawn_organic(w, true, 0);
    const EntityHandle tagged = spawn_organic(w, false, 1);
    Entity *e = w.registry.get(tagged);

    // The latch alone, free-standing, arms the fold [orig: @0x5a3bfe].
    CHECK(tag_radio_request(w, local, tagged));
    // No latch, no fold [orig: the `jz` off @0x5a3bfe -> `xor ebp, ebp`].
    e->radio_request = 0;
    CHECK(!tag_radio_request(w, local, tagged));
    e->radio_request = 1;

    // Aboard a def-type-1 carrier: cleared [orig: @0x5a3c0e -> @0x5a3c1a].
    const EntityHandle vehicle = spawn_link(w, 1);
    e->ground_target = vehicle;
    CHECK(!tag_radio_request(w, local, tagged));
    // A building floor (def type 2) is not a carrier ...
    const EntityHandle deck = spawn_link(w, 2);
    e->ground_target = deck;
    CHECK(tag_radio_request(w, local, tagged));
    // ... but the walk follows its groundEntity to the vehicle beneath
    // [orig: child = child->groundEntity @0x43bed7].
    w.registry.get(deck)->ground_target = vehicle;
    CHECK(!tag_radio_request(w, local, tagged));
    // A def-less link stops the walk before any later vehicle
    // [orig: the itemDef NULL stop @0x43bec5].
    const EntityHandle bare = spawn_link(w, 0, /*has_def=*/false);
    w.registry.get(deck)->ground_target = bare;
    w.registry.get(bare)->ground_target = vehicle;
    CHECK(tag_radio_request(w, local, tagged));
    // The 19-link bound [orig: `iteration >= 20` @0x43beca]: a vehicle as
    // the 19th link is found, as the 20th it is not.
    std::vector<EntityHandle> chain;
    for (int i = 0; i < 19; ++i) chain.push_back(spawn_link(w, 2));
    for (size_t i = 0; i + 1 < chain.size(); ++i)
        w.registry.get(chain[i])->ground_target = chain[i + 1];
    e->ground_target = chain[0];
    w.registry.get(chain[18])->ground_target = vehicle; // link 20 from the tag
    CHECK(friendly_tag_aboard_vehicle(w, chain[1]));    // ... link 19 from chain[1]
    CHECK(tag_radio_request(w, local, tagged));
    w.registry.get(chain[17])->ground_target = vehicle; // link 19
    CHECK(!tag_radio_request(w, local, tagged));
    // The walked field is the entity's OWN groundEntity: the drawer passes
    // the entity and the walk starts at its +0x28 [orig: @0x43bea4].
    e->ground_target = EntityHandle{};
    CHECK(tag_radio_request(w, local, tagged));
}

void test_viewer_gate() {
    Entity local;
    // On foot without a request: no viewer [orig: var_DC stays 0 @0x5a3bc1].
    CHECK(!friendly_tag_radio_request_viewer(local));
    // The two control seats [orig: `cmp ecx, 2` @0x5a3bd1; `cmp ecx, 5` @0x5a3bd6].
    local.mount_type = SeatType::Driver;
    CHECK(friendly_tag_radio_request_viewer(local));
    local.mount_type = SeatType::Controller;
    CHECK(friendly_tag_radio_request_viewer(local));
    // A passenger or gun seat is not one.
    local.mount_type = SeatType::Passenger;
    CHECK(!friendly_tag_radio_request_viewer(local));
    local.mount_type = SeatType::Gunner;
    CHECK(!friendly_tag_radio_request_viewer(local));
    // The local player's own +885 latch [orig: @0x5a3bdf].
    local.mount_type = SeatType::None;
    local.radio_request = 1;
    CHECK(friendly_tag_radio_request_viewer(local));
}

void test_neutral_organics_do_not_get_friendly_labels() {
    World w;
    w.registry.configure_pool(0, 8);
    const EntityHandle local = spawn_organic(w, true, 0);
    const EntityHandle neutral = spawn_organic(w, false, 0);
    const EntityHandle ally = spawn_organic(w, false, 0);
    w.registry.get(neutral)->team = 0;
    FriendlyTagPassContext ctx;
    ctx.game_type = 0x30020u;
    std::vector<FriendlyTagSource> tags;
    collect_friendly_tags(w, *w.registry.get(local), tags, ctx);
    CHECK(tags.size() == 1);
    if (tags.size() == 1) CHECK(tags[0].entity == ally);
    // The death-screen arm precedes the drawer's unequal-team rejection.
    ctx.death_screen = true;
    tags.clear();
    collect_friendly_tags(w, *w.registry.get(local), tags, ctx);
    CHECK(tags.size() == 2);
    // A team-0 local player passes the drawer's equal-team compare for team-0
    // neutrals and rejects the team-1 organic. [orig: @0x5a3c6b..0x5a3c95]
    ctx.death_screen = false;
    w.registry.get(local)->team = 0;
    tags.clear();
    collect_friendly_tags(w, *w.registry.get(local), tags, ctx);
    CHECK(tags.size() == 1);
    if (tags.size() == 1) CHECK(tags[0].entity == neutral);
}

} // namespace

int main() {
    test_tag_fold();
    test_viewer_gate();
    test_neutral_organics_do_not_get_friendly_labels();
    if (failures == 0) std::printf("friendly_tags_test: ok\n");
    return failures == 0 ? 0 : 1;
}
