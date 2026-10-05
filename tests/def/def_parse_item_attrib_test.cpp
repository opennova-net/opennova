// Test that items.def `attrib:` tokens parse into DefItemDef.attrib / attrib2 bits, plus
// the attrib arm's two non-bit side effects. The AIData bit (ItemDefAttrib & 0x100000) is
// the AI-class flag that gates the 0x0D AI-trailer (D-NET-97); PlayerControl (0x40),
// EWeap (0x20), S&D (0x8000, the S&D/A&D objective target) and a few attrib2 tokens are
// spot-checked alongside it; `Door` defaults the door count and `Parent` writes the
// ItemDef+0x548 byte. [orig: ItemDef_ParseProperty @0x49eb00 (S&D @0x4a084e..0x4a086d,
// Door @0x4a0caa..0x4a0cc0, Parent @0x4a0cd6..0x4a0ce2); docs/world/itemdef-re.md]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <formats/def/def.h>

using namespace opennova::def;

static int failures = 0;
#define CHECK(c) \
    do { if (!(c)) { fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static const DefItemDef *find_by_id(const DefItemsFile *items, int id) {
    for (size_t i = 0; i < items->count; ++i) {
        if (items->entries[i].id == id) return &items->entries[i];
    }
    return NULL;
}

int main(void) {
    // The first block carries an AI-capable, player-controllable attrib line (mirrors the
    // real JOX/ITEMS.DEF Player block); the second is a non-AI emplaced weapon; then the
    // JOX `attrib: S&D staticdeath` spelling (the tokenizer keeps the '&'), its lowercase
    // twin, the `Door` default in isolation and around an authored num_doors, and `Parent`.
    const char *test_def =
        "\r\n"
        "begin \"AI Player\"\r\n"
        "  id 1001\r\n"
        "  type person\r\n"
        "  attrib: exp1 noscar AIData PlayerControl PilotOnly DynamicShadow EWeap Missile\r\n"
        "end\r\n"
        "begin \"Plain Weapon\"\r\n"
        "  id 1002\r\n"
        "  type object\r\n"
        "  attrib: EWeap StaticShadow\r\n"
        "end\r\n"
        "begin \"SD Target\"\r\n"
        "  id 1003\r\n"
        "  type building\r\n"
        "  attrib: S&D staticdeath\r\n"
        "end\r\n"
        "begin \"SD Lower\"\r\n"
        "  id 1004\r\n"
        "  type building\r\n"
        "  attrib: s&d\r\n"
        "end\r\n"
        "begin \"Door Default\"\r\n"
        "  id 1005\r\n"
        "  type building\r\n"
        "  attrib: Door\r\n"
        "end\r\n"
        "begin \"Door After Count\"\r\n"
        "  id 1006\r\n"
        "  type building\r\n"
        "  num_doors 3\r\n"
        "  attrib: Door\r\n"
        "end\r\n"
        "begin \"Door Before Count\"\r\n"
        "  id 1007\r\n"
        "  type building\r\n"
        "  attrib: Door\r\n"
        "  num_doors 3\r\n"
        "end\r\n"
        "begin \"Parent Carrier\"\r\n"
        "  id 1008\r\n"
        "  type vehicle\r\n"
        "  attrib: Parent\r\n"
        "end\r\n";

    DefItemsFile items;
    memset(&items, 0, sizeof(items));
    if (def_parse_items_memory((const unsigned char *)test_def, strlen(test_def), &items) != 0) {
        fprintf(stderr, "FAIL: def_parse_items_memory failed\n");
        return 1;
    }
    CHECK(items.count == 8);

    const DefItemDef *ai = find_by_id(&items, 1001);
    const DefItemDef *plain = find_by_id(&items, 1002);
    const DefItemDef *sd = find_by_id(&items, 1003);
    const DefItemDef *sd_lower = find_by_id(&items, 1004);
    const DefItemDef *door = find_by_id(&items, 1005);
    const DefItemDef *door_after = find_by_id(&items, 1006);
    const DefItemDef *door_before = find_by_id(&items, 1007);
    const DefItemDef *parent = find_by_id(&items, 1008);
    CHECK(ai && plain && sd && sd_lower && door && door_after && door_before && parent);
    if (failures) {
        def_free_items(&items);
        return 1;
    }

    // AIData -> 0x100000; PlayerControl -> 0x40; EWeap -> 0x20; Missile -> 0x400;
    // NoScar -> 0x10000000; attrib2: DynamicShadow -> 0x10. Unknown tokens (exp1,
    // PilotOnly) stay unmapped (not in the witnessed map); no S&D bit, no Parent byte.
    CHECK(ai->attrib & DEF_ITEM_ATTRIB_AIDATA);
    CHECK(ai->attrib & DEF_ITEM_ATTRIB_PLAYERCONTROL);
    CHECK(ai->attrib & DEF_ITEM_ATTRIB_EWEAP);
    CHECK(ai->attrib & DEF_ITEM_ATTRIB_MISSILE);
    CHECK(ai->attrib & DEF_ITEM_ATTRIB_NOSCAR);
    CHECK(ai->attrib2 & DEF_ITEM_ATTRIB2_DYNAMICSHADOW);
    CHECK((ai->attrib & DEF_ITEM_ATTRIB_SD) == 0);
    CHECK(ai->attrib_parent == 0);

    // Non-AI: AIData must be CLEAR; EWeap set; StaticShadow -> attrib2 0x20.
    CHECK((plain->attrib & DEF_ITEM_ATTRIB_AIDATA) == 0);
    CHECK(plain->attrib & DEF_ITEM_ATTRIB_EWEAP);
    CHECK(plain->attrib2 & DEF_ITEM_ATTRIB2_STATICSHADOW);
    CHECK(plain->attrib_parent == 0);

    // S&D -> 0x8000, whole-token and case-insensitive, beside its attrib2 neighbour.
    CHECK(DEF_ITEM_ATTRIB_SD == 0x8000u);
    CHECK(sd->attrib & DEF_ITEM_ATTRIB_SD);
    CHECK(sd->attrib2 & DEF_ITEM_ATTRIB2_STATICDEATH);
    CHECK(sd_lower->attrib & DEF_ITEM_ATTRIB_SD);
    CHECK(sd_lower->attrib2 == 0u);

    // Door -> 0x80 plus the door-count default: the low byte of the polymorphic
    // deathtime dword becomes 1 only while it is still 0 [orig: @0x4a0cb0..0x4a0cb9];
    // an authored num_doors wins in either order.
    CHECK(door->attrib & DEF_ITEM_ATTRIB_DOOR);
    CHECK((door->deathtime_ticks & 0xFF) == 1);
    CHECK(door_after->attrib & DEF_ITEM_ATTRIB_DOOR);
    CHECK((door_after->deathtime_ticks & 0xFF) == 3);
    CHECK(door_before->attrib & DEF_ITEM_ATTRIB_DOOR);
    CHECK((door_before->deathtime_ticks & 0xFF) == 3);

    // Parent -> the +0x548 byte only: no attrib/attrib2 bit.
    CHECK(parent->attrib_parent == 1);
    CHECK(parent->attrib == 0u);
    CHECK(parent->attrib2 == 0u);

    def_free_items(&items);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("PASS: item attrib parsing OK\n");
    return 0;
}
