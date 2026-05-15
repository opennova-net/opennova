#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "common/test_paths.h"
#include "threedi/threedi_3di3.h"

static int failures = 0;

static void expect_true(const char *label, int cond) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", label);
        failures++;
    }
}

static void expect_eq_str(const char *label, const char *got, const char *want) {
    if (std::strcmp(got, want) != 0) {
        std::fprintf(stderr, "FAIL: %s got '%s', want '%s'\n", label, got, want);
        failures++;
    }
}

int main(void) {
    const char *repo_root = test_paths_repo_root(__FILE__);
    char gp_path[4096];
    std::snprintf(gp_path, sizeof(gp_path), "%s/fixtures/threedi/gp/wcrate5.3di", repo_root);

    Threedi3di3 model;
    std::memset(&model, 0, sizeof(model));
    if (threedi_read_model_auto(gp_path, &model) != 0) {
        std::fprintf(stderr, "FAIL: threedi_read_model_auto failed for %s\n", gp_path);
        return EXIT_FAILURE;
    }

    expect_true("auto GP version", model.version == 259);
    expect_eq_str("auto GP model name", model.header.name, "wcrate5");
    expect_true("auto GP has LODs", model.lod_count > 0);
    expect_true("auto GP has materials", model.material_count > 0);
    expect_true("auto GP first LOD has vertices", model.lods[0].vertices.count > 0);
    expect_true("auto GP first LOD has indices", model.lods[0].indices.count > 0);

    int saw_bhd_shader_tag = 0;
    for (uint32_t i = 0; i < model.material_count; ++i) {
        if (std::strncmp(model.materials[i].shader_name, "VS_", 3) == 0 ||
            std::strcmp(model.materials[i].shader_name, "FFP_GLASS") == 0) {
            saw_bhd_shader_tag = 1;
            break;
        }
    }
    expect_true("auto GP uses BHD shader synthesis", saw_bhd_shader_tag);

    threedi_3di3_free(&model);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
