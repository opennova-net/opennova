#include "object/bake.h"
#include "object/export_3di.h"
#include "threedi/threedi.h"
#include "threedi/threedi_3di3.h"
#include "common/test_expect.h"
#include "common/test_paths.h"

#include <cstring>
#include <cstdio>
#include <string>

int main() {
    const char* repo_root = test_paths_repo_root(__FILE__);
    const char* tmp_root = test_paths_temp_dir();

    char project_path[4096];
    std::snprintf(project_path, sizeof(project_path),
                  "%s/fixtures/3dp/Bird1/Bird1.3dp", repo_root);

    char output_path[4096];
    std::snprintf(output_path, sizeof(output_path),
                  "%s%cobject_bake_project_export_Bird1.3di",
                  tmp_root, TEST_PATHS_SEP);

    const BakeStatus status = bake_project_export(project_path, output_path, "Bird1",
                                                  BAKE_UPDATE_ALL);
    TEST_EXPECT(status == BAKE_STATUS_OK);

    ThreediFile file{};
    TEST_EXPECT(threedi_read_file(output_path, &file) == 0);
    TEST_EXPECT(file.root != nullptr);
    TEST_EXPECT(file.root->child_count > 0);
    threedi_free_file(&file);

    opennova::object::LodBucketWorkspace workspace{};
    opennova::object::UserPoint user_point{};
    user_point.pos[0] = 3961.0f / 65536.0f;
    user_point.pos[1] = 0.0f;
    user_point.pos[2] = 16456.0f / 65536.0f;
    user_point.axis[2][0] = 1.0f;
    user_point.axis[2][1] = 0.0f;
    user_point.axis[2][2] = 0.0f;
    user_point.subObj = 14;
    user_point.type = static_cast<uint32_t>('S');
    std::strncpy(user_point.name, "Look", sizeof(user_point.name) - 1);

    workspace.lod.userPointCount = 1;
    workspace.lod.userPoints = &user_point;

    Threedi3di3 model{};
    opennova::object::Export3diOptions options{};
    options.model_name = "usrp_scale";
    std::string error;
    TEST_EXPECT(opennova::object::build_3di_model(workspace, nullptr, options, model, error));
    TEST_EXPECT(model.user_point_count == 1);
    TEST_EXPECT(model.user_points != nullptr);
    TEST_EXPECT(model.user_points[0].x == 3961);
    TEST_EXPECT(model.user_points[0].y == 0);
    TEST_EXPECT(model.user_points[0].z == 16456);
    TEST_EXPECT(model.user_points[0].rot_x == 65536);
    TEST_EXPECT(model.user_points[0].rot_y == 0);
    TEST_EXPECT(model.user_points[0].rot_z == 0);
    TEST_EXPECT(model.user_points[0].subobject_index == 14);
    TEST_EXPECT(model.user_points[0].userpoint_type == static_cast<int32_t>('S'));
    TEST_EXPECT(std::strncmp(model.user_points[0].name, "Look", 4) == 0);
    threedi_3di3_free(&model);

    return 0;
}
