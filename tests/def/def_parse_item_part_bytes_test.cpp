// The byte-writing items.def keys over the polymorphic ItemDef +0x890..+0x897
// run the door (+0x890 dword, deathtime_ticks) and clipsize (+0x894 dword)
// fields share: num_doors/first_door/first_subobject clamp the low byte of
// atol read signed to 0..30 (the first_* keys one less), rotor_parts and
// aux_parts store four raw low bytes. The render reads +0x891/+0x892 as the
// building's forced-visible section bases.
// [orig: ItemDef_ParseProperty — num_doors @0x49F766..0x49F78A, first_door
// @0x49F7BA..0x49F7DE, first_subobject @0x49F9B0..0x49F9CC, rotor_parts
// @0x49EF5D..0x49EFB6, aux_parts @0x49EFF3..0x49F04C; Terrain_RenderSectorModels
// @0x5c5d7c..0x5c5da8]
#include <stdio.h>
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

static unsigned byte_of(int dword, int index) {
    return (static_cast<unsigned>(dword) >> (index * 8)) & 0xFFu;
}

int main(void) {
    const char *test_def =
        "begin \"Fort piece\"\r\n"          // the JO Ijava05 shape: First_Door 4
        "  id 2001\r\n"
        "  type building\r\n"
        "  First_Door 4\r\n"
        "end\r\n"
        "begin \"Sub objects\"\r\n"
        "  id 2002\r\n"
        "  type building\r\n"
        "  num_doors 1\r\n"
        "  first_door 3\r\n"
        "  first_subobject 6\r\n"
        "end\r\n"
        "begin \"Clamps\"\r\n"
        "  id 2003\r\n"
        "  type building\r\n"
        "  num_doors 200\r\n"                // low byte 0xC8 reads -56 -> 0
        "  first_door 300\r\n"               // low byte 44, minus one -> 43 -> 30
        "  first_subobject 0\r\n"            // -1 -> 0
        "end\r\n"
        "begin \"Helicopter\"\r\n"
        "  id 2004\r\n"
        "  type vehicle\r\n"
        "  rotor_parts 1 2 3 4\r\n"
        "  aux_parts 5 6 7 8\r\n"
        "end\r\n";

    DefItemsFile items;
    memset(&items, 0, sizeof(items));
    if (def_parse_items_memory((const unsigned char *)test_def, strlen(test_def), &items) != 0) {
        fprintf(stderr, "FAIL: def_parse_items_memory failed\n");
        return 1;
    }
    const DefItemDef *fort = find_by_id(&items, 2001);
    const DefItemDef *sub = find_by_id(&items, 2002);
    const DefItemDef *clamps = find_by_id(&items, 2003);
    const DefItemDef *heli = find_by_id(&items, 2004);
    CHECK(fort && sub && clamps && heli);
    if (failures) {
        def_free_items(&items);
        return 1;
    }

    // first_door 4 -> +0x891 = 3 (sections >= 3 draw forced), plus the Door
    // attrib and the num_doors default of 1 the attrib arm leaves alone here.
    CHECK(byte_of(fort->deathtime_ticks, 1) == 3);
    CHECK(fort->attrib & DEF_ITEM_ATTRIB_DOOR);

    // first_subobject 6 -> +0x892 = 5, without touching the door bytes or
    // adding an attrib bit of its own.
    CHECK(byte_of(sub->deathtime_ticks, 0) == 1);
    CHECK(byte_of(sub->deathtime_ticks, 1) == 2);
    CHECK(byte_of(sub->deathtime_ticks, 2) == 5);

    // The signed-byte clamps.
    CHECK(byte_of(clamps->deathtime_ticks, 0) == 0);
    CHECK(byte_of(clamps->deathtime_ticks, 1) == 30);
    CHECK(byte_of(clamps->deathtime_ticks, 2) == 0);

    // rotor_parts -> +0x890, +0x891, +0x894, +0x895; aux_parts -> +0x896,
    // +0x897, +0x892, +0x893.
    CHECK(byte_of(heli->deathtime_ticks, 0) == 1);
    CHECK(byte_of(heli->deathtime_ticks, 1) == 2);
    CHECK(byte_of(heli->deathtime_ticks, 2) == 7);
    CHECK(byte_of(heli->deathtime_ticks, 3) == 8);
    CHECK(byte_of(heli->clipsize, 0) == 3);
    CHECK(byte_of(heli->clipsize, 1) == 4);
    CHECK(byte_of(heli->clipsize, 2) == 5);
    CHECK(byte_of(heli->clipsize, 3) == 6);
    CHECK((heli->attrib & DEF_ITEM_ATTRIB_DOOR) == 0);

    def_free_items(&items);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("PASS: item part-byte parsing OK\n");
    return 0;
}
