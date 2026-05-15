// tests/object/bones_to_ase_with_bad_test.cpp
//
// Phase G regression: object_emit_bone_objects_with_bad emits N+1 bones from
// a BadFile (N BAD bones + root_motion). No paired stock fixture exists for
// end-to-end byte-match testing; this synthetic test pins the algorithm.

#include "object/bones_to_ase.h"
#include "bad/bad.h"
#include "ase/ase.h"
#include "ase/types.h"
#include "threedi/threedi_3di3.h"
#include "common/test_expect.h"

#include <cstdio>
#include <cstring>

int main() {
    // Build a minimal IR (lod_count = 1, render_object_count = 0; the BAD
    // path doesn't use IR's render_objects, but the function checks lod_count).
    Threedi3di3 ir;
    std::memset(&ir, 0, sizeof(ir));

    ThreediLod lod0;
    std::memset(&lod0, 0, sizeof(lod0));
    lod0.render_object_count = 0;
    lod0.render_objects = nullptr;

    ir.lod_count = 1;
    ir.lods = &lod0;

    // Build a 2-bone BadFile: BN01 (root, position (1, 2, 3)),
    // SubBone (parent=BN01, position (4, 5, 6)).
    BadFile bf;
    std::memset(&bf, 0, sizeof(bf));

    BadBone bones[2];
    std::memset(bones, 0, sizeof(bones));

    std::snprintf(bones[0].name, sizeof(bones[0].name), "BN01");
    bones[0].parent_index = -1;
    bones[0].position[0] = 1.0f;
    bones[0].position[1] = 2.0f;
    bones[0].position[2] = 3.0f;

    std::snprintf(bones[1].name, sizeof(bones[1].name), "SubBone");
    bones[1].parent_index = 0;
    bones[1].position[0] = 4.0f;
    bones[1].position[1] = 5.0f;
    bones[1].position[2] = 6.0f;

    bf.bones = bones;
    bf.num_bones = 2;

    // Capacity: 3 (2 BAD bones + 1 root_motion).
    ase_Object out[3];
    std::memset(out, 0, sizeof(out));

    int n = object_emit_bone_objects_with_bad(&ir, &bf, out, 3);
    TEST_EXPECT(n == 3);

    // out[0]: BN01, parent="root_motion" (parent_index=-1 + root_motion exists).
    // Per pre-deletion Python at ase_from_3di3.py:638-639:
    //   "parent = ''; if parents[i] >= 0: parent = _bone_export_name(names[parents[i]])
    //    elif name != 'root_motion' and 'root_motion' in names: parent = 'root_motion'"
    TEST_EXPECT(std::strcmp(out[0].name, "BN01") == 0);
    TEST_EXPECT(std::strcmp(out[0].parent_name, "root_motion") == 0);

    // out[1]: SubBone, parent=BN01 (parent_index=0 resolves to names[0]="BN01").
    TEST_EXPECT(std::strcmp(out[1].name, "SubBone") == 0);
    TEST_EXPECT(std::strcmp(out[1].parent_name, "BN01") == 0);

    // out[2]: root_motion, parent="" (root bone has no parent).
    TEST_EXPECT(std::strcmp(out[2].name, "root_motion") == 0);
    TEST_EXPECT(out[2].parent_name[0] == '\0');

    // Also pin: no-BAD path (passing bad_file=NULL) returns lod0.render_object_count = 0.
    ase_Object out2[1];
    std::memset(out2, 0, sizeof(out2));
    int n2 = object_emit_bone_objects_with_bad(&ir, nullptr, out2, 1);
    TEST_EXPECT(n2 == 0);

    std::fprintf(stdout,
                 "PASS: object_emit_bone_objects_with_bad emits %d bones (2 BAD + root_motion); "
                 "no-BAD fallback returns %d.\n", n, n2);
    return 0;
}
