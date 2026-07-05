// Pins the FULL items.def `type` token -> ItemDefType value mapping the engine
// stores at ItemDef+0x5C. The mapping is the witnessed original
// [orig: ItemDef_ParseProperty @ 0x49eb00; docs/world/itemdef-re.md D-ITEMDEF-1]:
//   vehicle=1, decoration=2, foliage=2, person=3, marker=4, building=5,
//   powerup=6, object=6, effect=8; an unknown token -> 0 (unset).
// It is deliberately NON-INJECTIVE (decoration/foliage share 2, powerup/object
// share 6). item_type_from_string is file-static, so we exercise it through the
// public def_parse_items_memory parser over a tiny in-memory items.def.
#include <stdio.h>
#include <string.h>

#include "def/def.h"

static int type_of(const char *type_token) {
    char buf[256];
    // One minimal begin/end block whose only body line is the `type` token.
    int n = snprintf(buf, sizeof(buf), "begin \"probe\"\n  type %s\nend\n", type_token);

    DefItemsFile items;
    memset(&items, 0, sizeof(items));
    if (def_parse_items_memory((const unsigned char *)buf, (size_t)n, &items) != 0) {
        fprintf(stderr, "FAIL: def_parse_items_memory failed for token '%s'\n", type_token);
        return -1000;
    }
    if (items.count != 1) {
        fprintf(stderr, "FAIL: expected 1 entry for token '%s', got %zu\n", type_token, items.count);
        def_free_items(&items);
        return -1000;
    }
    int t = items.entries[0].type;
    def_free_items(&items);
    return t;
}

static int expect(const char *token, int want) {
    int got = type_of(token);
    if (got != want) {
        fprintf(stderr, "FAIL: type '%s' -> %d, expected %d\n", token, got, want);
        return 1;
    }
    return 0;
}

int main(void) {
    int fails = 0;

    // The witnessed engine mapping (docs/world/itemdef-re.md D-ITEMDEF-1).
    fails += expect("vehicle", DEF_ITEM_TYPE_VEHICLE);       // 1
    fails += expect("decoration", DEF_ITEM_TYPE_DECORATION); // 2
    fails += expect("foliage", DEF_ITEM_TYPE_FOLIAGE);       // 2 (shared with decoration)
    fails += expect("person", DEF_ITEM_TYPE_PERSON);         // 3
    fails += expect("marker", DEF_ITEM_TYPE_MARKER);         // 4
    fails += expect("building", DEF_ITEM_TYPE_BUILDING);     // 5
    fails += expect("powerup", DEF_ITEM_TYPE_POWERUP);       // 6
    fails += expect("object", DEF_ITEM_TYPE_OBJECT);         // 6 (shared with powerup)
    fails += expect("effect", DEF_ITEM_TYPE_EFFECT);         // 8

    // Case-insensitive, mirroring the original's _stricmp chain.
    fails += expect("VEHICLE", DEF_ITEM_TYPE_VEHICLE);
    fails += expect("Person", DEF_ITEM_TYPE_PERSON);

    // Non-injective pairs really do collide on the same value.
    if (DEF_ITEM_TYPE_DECORATION != DEF_ITEM_TYPE_FOLIAGE) {
        fprintf(stderr, "FAIL: decoration/foliage must share a value\n");
        fails += 1;
    }
    if (DEF_ITEM_TYPE_POWERUP != DEF_ITEM_TYPE_OBJECT) {
        fprintf(stderr, "FAIL: powerup/object must share a value\n");
        fails += 1;
    }

    // Unknown token -> unset (0); 7 is unused by the engine.
    fails += expect("frobnicate", DEF_ITEM_TYPE_UNSET);      // 0

    if (fails != 0) {
        fprintf(stderr, "FAIL: %d item-type mapping check(s) failed\n", fails);
        return 1;
    }
    printf("PASS: items.def type mapping matches the witnessed engine values\n");
    return 0;
}
