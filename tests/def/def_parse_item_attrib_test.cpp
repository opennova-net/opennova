// Test that items.def `attrib:` tokens parse into DefItemDef.attrib / attrib2 bits.
// The AIData bit (ItemDefAttrib & 0x100000) is the AI-class flag that gates the 0x0D
// AI-trailer (D-NET-97); PlayerControl (0x40), EWeap (0x20), and a few attrib2 tokens are
// spot-checked alongside it. [orig: ItemDef_ParseProperty @0x49eb00; docs/world/itemdef-re.md]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "def/def.h"

static const DefItemDef *find_by_id(const DefItemsFile *items, int id) {
    for (size_t i = 0; i < items->count; ++i) {
        if (items->entries[i].id == id) return &items->entries[i];
    }
    return NULL;
}

int main(void) {
    // Two item blocks. The first carries an AI-capable, player-controllable attrib line
    // (mirrors the real JOX/ITEMS.DEF Player block); the second is a non-AI emplaced weapon.
    const char *test_def =
        "\n"
        "begin \"AI Player\"\n"
        "  id 1001\n"
        "  type person\n"
        "  attrib: exp1 noscar AIData PlayerControl PilotOnly DynamicShadow EWeap Missile\n"
        "end\n"
        "begin \"Plain Weapon\"\n"
        "  id 1002\n"
        "  type object\n"
        "  attrib: EWeap StaticShadow\n"
        "end\n";

    DefItemsFile items;
    memset(&items, 0, sizeof(items));
    if (def_parse_items_memory((const unsigned char *)test_def, strlen(test_def), &items) != 0) {
        fprintf(stderr, "FAIL: def_parse_items_memory failed\n");
        return 1;
    }
    if (items.count != 2) {
        fprintf(stderr, "FAIL: expected 2 entries, got %zu\n", items.count);
        def_free_items(&items);
        return 1;
    }

    const DefItemDef *ai = find_by_id(&items, 1001);
    if (!ai) {
        fprintf(stderr, "FAIL: could not find AI Player (id 1001)\n");
        def_free_items(&items);
        return 1;
    }
    // AIData -> 0x100000 set; PlayerControl -> 0x40 set; EWeap -> 0x20; Missile -> 0x400;
    // NoScar -> 0x10000000. attrib2: DynamicShadow -> 0x10. Unknown tokens (exp1, PilotOnly)
    // stay unmapped (not in the witnessed map).
    if (!(ai->attrib & 0x100000u)) {
        fprintf(stderr, "FAIL: AI Player should have AIData (0x100000), attrib=0x%08x\n", ai->attrib);
        def_free_items(&items);
        return 1;
    }
    if (!(ai->attrib & 0x40u)) {
        fprintf(stderr, "FAIL: AI Player should have PlayerControl (0x40), attrib=0x%08x\n", ai->attrib);
        def_free_items(&items);
        return 1;
    }
    if (!(ai->attrib & 0x20u)) {
        fprintf(stderr, "FAIL: AI Player should have EWeap (0x20), attrib=0x%08x\n", ai->attrib);
        def_free_items(&items);
        return 1;
    }
    if (!(ai->attrib & 0x400u)) {
        fprintf(stderr, "FAIL: AI Player should have Missile (0x400), attrib=0x%08x\n", ai->attrib);
        def_free_items(&items);
        return 1;
    }
    if (!(ai->attrib & 0x10000000u)) {
        fprintf(stderr, "FAIL: AI Player should have NoScar (0x10000000), attrib=0x%08x\n", ai->attrib);
        def_free_items(&items);
        return 1;
    }
    if (!(ai->attrib2 & 0x10u)) {
        fprintf(stderr, "FAIL: AI Player should have DynamicShadow attrib2 (0x10), attrib2=0x%08x\n",
                ai->attrib2);
        def_free_items(&items);
        return 1;
    }

    const DefItemDef *plain = find_by_id(&items, 1002);
    if (!plain) {
        fprintf(stderr, "FAIL: could not find Plain Weapon (id 1002)\n");
        def_free_items(&items);
        return 1;
    }
    // Non-AI: AIData (0x100000) must be CLEAR; EWeap (0x20) set; StaticShadow -> attrib2 0x20.
    if (plain->attrib & 0x100000u) {
        fprintf(stderr, "FAIL: Plain Weapon should NOT have AIData (0x100000), attrib=0x%08x\n",
                plain->attrib);
        def_free_items(&items);
        return 1;
    }
    if (!(plain->attrib & 0x20u)) {
        fprintf(stderr, "FAIL: Plain Weapon should have EWeap (0x20), attrib=0x%08x\n", plain->attrib);
        def_free_items(&items);
        return 1;
    }
    if (!(plain->attrib2 & 0x20u)) {
        fprintf(stderr, "FAIL: Plain Weapon should have StaticShadow attrib2 (0x20), attrib2=0x%08x\n",
                plain->attrib2);
        def_free_items(&items);
        return 1;
    }

    def_free_items(&items);
    printf("PASS: item attrib parsing OK\n");
    return 0;
}
