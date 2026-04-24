#include "bad/bad.h"

#include <cassert>
#include <cstdio>
#include <cstring>

#include "common/test_paths.h"

int main() {
    char path[4096];
    std::snprintf(path, sizeof(path), "%s%cfixtures%cbad%cBINOC.bad",
                  test_paths_repo_root(__FILE__), TEST_PATHS_SEP,
                  TEST_PATHS_SEP, TEST_PATHS_SEP);

    BadFile file;
    assert(bad_parse(path, &file) == 0);

    assert(file.version == 1);
    assert(file.header_size == 80);
    assert(file.fps == 30);
    assert(file.frame_count == 3);
    assert(file.flags == 1);
    assert(file.bone_count == 19);

    assert(file.num_bones == 19);
    assert(file.bones != nullptr);
    assert(std::strcmp(file.bones[0].name, "BN01 Hips") == 0);
    assert(file.bones[0].parent_index == -1);

    assert(file.num_channels == 19);
    assert(file.channels != nullptr);
    assert(file.channels[0].frame_count == 4);
    assert(file.channels[0].frame_lengths != nullptr);
    assert(file.channels[0].rotations != nullptr);

    assert(file.num_events == 4);
    assert(file.events != nullptr);
    assert(file.num_translations == 0);
    assert(file.translations == nullptr);

    bad_free(&file);
    assert(file.bones == nullptr);
    assert(file.channels == nullptr);
    assert(file.events == nullptr);

    BadFile invalid;
    assert(bad_parse(nullptr, &invalid) == -1);

    return 0;
}
