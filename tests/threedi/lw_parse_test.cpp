#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/test_paths.h"
#include "threedi/threedi_ir.h"
#include "threedi/threedi_lw.h"

int main(void) {
    const char *repo_root = test_paths_repo_root(__FILE__);
    char path[4096];
    snprintf(path, sizeof(path), "%s/fixtures/lw/dflw/badguy/BADGUY.3DI", repo_root);

    ThreediLwFile lw;
    threedi_lw_init(&lw);
    if (threedi_lw_read(path, &lw) != 0) {
        fprintf(stderr, "FAIL: threedi_lw_read failed for %s\n", path);
        return EXIT_FAILURE;
    }

    if (lw.version != 10 ||
        strcmp(lw.name, "badguy") != 0 ||
        lw.lod_count != 4 ||
        lw.material_count != 7 ||
        lw.lods[0].vertex_count != 407 ||
        lw.lods[0].normal_count != 823 ||
        lw.lods[0].face_count != 492 ||
        lw.lods[0].subobject_count != 15 ||
        lw.lods[0].surface_count != 8) {
        fprintf(stderr,
                "FAIL: badguy LW summary mismatch version=%u name=%s lods=%u mats=%u "
                "lod0 verts=%u normals=%u faces=%u subobjs=%u surfaces=%u\n",
                (unsigned)lw.version,
                lw.name,
                (unsigned)lw.lod_count,
                (unsigned)lw.material_count,
                (unsigned)lw.lods[0].vertex_count,
                (unsigned)lw.lods[0].normal_count,
                (unsigned)lw.lods[0].face_count,
                (unsigned)lw.lods[0].subobject_count,
                (unsigned)lw.lods[0].surface_count);
        threedi_lw_free(&lw);
        return EXIT_FAILURE;
    }

    if (strcmp(lw.materials[0].texture0, "ADstCamo.pcx") != 0 ||
        strcmp(lw.materials[6].texture0, "Hfeet01.PCX") != 0) {
        fprintf(stderr, "FAIL: material names not decoded: '%s' '%s'\n",
                lw.materials[0].texture0,
                lw.materials[6].texture0);
        threedi_lw_free(&lw);
        return EXIT_FAILURE;
    }

    ThreediModelIR ir;
    threedi_ir_init(&ir);
    if (threedi_ir_read(path, &ir) != 0) {
        fprintf(stderr, "FAIL: threedi_ir_read rejected LW model %s\n", path);
        threedi_lw_free(&lw);
        return EXIT_FAILURE;
    }

    if (ir.source_format != THREEDI_IR_SOURCE_LW ||
        ir.mesh_type != THREEDI_IR_MESH_BASIC ||
        ir.lod_count != 4 ||
        ir.material_count != 7 ||
        ir.lods[0].part_count != 15 ||
        ir.lods[0].primitive_count != 17 ||
        ir.lods[0].index_count == 0 ||
        ir.lods[0].vertex_count == 0) {
        fprintf(stderr,
                "FAIL: LW IR mismatch source=%d mesh=%d lods=%zu mats=%zu parts=%zu prims=%zu verts=%zu indices=%zu\n",
                ir.source_format,
                ir.mesh_type,
                ir.lod_count,
                ir.material_count,
                ir.lods[0].part_count,
                ir.lods[0].primitive_count,
                ir.lods[0].vertex_count,
                ir.lods[0].index_count);
        threedi_ir_free(&ir);
        threedi_lw_free(&lw);
        return EXIT_FAILURE;
    }

    threedi_ir_free(&ir);
    threedi_lw_free(&lw);
    printf("PASS: LW badguy parse and IR conversion OK\n");
    return EXIT_SUCCESS;
}
