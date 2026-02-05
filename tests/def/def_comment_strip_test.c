// Test that trailing // comments are stripped from DEF entry values.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "def/def.h"
#include "common/test_paths.h"

int main(void) {
    const char *test_def =
        "\n"
        "begin \"Test Item With Comments\"\n"
        "  id 999999\n"
        "  type person\n"
        "  graphic TestModel\t\t\t// this comment should be stripped\n"
        "  sid test_sid\t\t\t\t// another trailing comment\n"
        "  anim_def TestAnim\t\t\t// yet another comment\n"
        "  hp 100\t\t\t\t\t//was 150\n"
        "  sound_profile SP_Test\t\t// Male Sound Profile\n"
        "end\n";

    char temp_path[4096];
    snprintf(temp_path, sizeof(temp_path), "%s/def_comment_strip_test.def", test_paths_temp_dir());
    FILE *f = fopen(temp_path, "wb");
    if (!f) {
        fprintf(stderr, "FAIL: could not create temp file\n");
        return 1;
    }
    fwrite(test_def, 1, strlen(test_def), f);
    fclose(f);

    DefItemsFile items;
    memset(&items, 0, sizeof(items));
    if (def_parse_items(temp_path, &items) != 0) {
        fprintf(stderr, "FAIL: def_parse_items failed\n");
        remove(temp_path);
        return 1;
    }

    if (items.count != 1) {
        fprintf(stderr, "FAIL: expected 1 entry, got %zu\n", items.count);
        def_free_items(&items);
        remove(temp_path);
        return 1;
    }

    const DefItemDef *item = &items.entries[0];

    if (strcmp(item->graphic, "TestModel") != 0) {
        fprintf(stderr, "FAIL: graphic mismatch: expected 'TestModel', got '%s'\n", item->graphic);
        fprintf(stderr, "  (trailing comment was not stripped)\n");
        def_free_items(&items);
        remove(temp_path);
        return 1;
    }

    if (strcmp(item->sid, "test_sid") != 0) {
        fprintf(stderr, "FAIL: sid mismatch: expected 'test_sid', got '%s'\n", item->sid);
        def_free_items(&items);
        remove(temp_path);
        return 1;
    }

    if (strcmp(item->anim_def, "TestAnim") != 0) {
        fprintf(stderr, "FAIL: anim_def mismatch: expected 'TestAnim', got '%s'\n", item->anim_def);
        def_free_items(&items);
        remove(temp_path);
        return 1;
    }

    if (item->hp != 100) {
        fprintf(stderr, "FAIL: hp mismatch: expected 100, got %d\n", item->hp);
        def_free_items(&items);
        remove(temp_path);
        return 1;
    }

    if (strcmp(item->sound_profile, "SP_Test") != 0) {
        fprintf(stderr, "FAIL: sound_profile mismatch: expected 'SP_Test', got '%s'\n",
                item->sound_profile);
        def_free_items(&items);
        remove(temp_path);
        return 1;
    }

    def_free_items(&items);
    remove(temp_path);
    printf("PASS: comment stripping OK\n");
    return 0;
}
