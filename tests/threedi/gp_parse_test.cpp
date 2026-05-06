// GP format parsing test - verifies GP parser handles files correctly.
// Uses fixtures/threedi/gp directory.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/dirent_compat.h"
#include <sys/stat.h>

#include "threedi/threedi_gp.h"
#include "threedi/threedi_ir.h"
#include "common/test_paths.h"

static int has_extension(const char *name, const char *ext) {
    size_t nlen = strlen(name);
    size_t elen = strlen(ext);
    if (nlen < elen) return 0;
    return strcmp(name + nlen - elen, ext) == 0;
}

static int is_gp_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t magic[4];
    size_t n = fread(magic, 1, 4, f);
    fclose(f);
    if (n < 3) return 0;
    return memcmp(magic, "GPM", 3) == 0 ||
           memcmp(magic, "GPS", 3) == 0 ||
           memcmp(magic, "GPP", 3) == 0;
}

static float dot3(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static int ir_triangle_opposes_authored_normals(const ThreediIRLod *lod,
                                                uint32_t vertex_offset,
                                                uint16_t a,
                                                uint16_t b,
                                                uint16_t c) {
    uint32_t ia = vertex_offset + a;
    uint32_t ib = vertex_offset + b;
    uint32_t ic = vertex_offset + c;
    if ((size_t)ia >= lod->vertex_count ||
        (size_t)ib >= lod->vertex_count ||
        (size_t)ic >= lod->vertex_count) {
        return 0;
    }

    const ThreediIRVertex *va = &lod->vertices[ia];
    const ThreediIRVertex *vb = &lod->vertices[ib];
    const ThreediIRVertex *vc = &lod->vertices[ic];

    float e1[3] = {
        vb->position[0] - va->position[0],
        vb->position[1] - va->position[1],
        vb->position[2] - va->position[2],
    };
    float e2[3] = {
        vc->position[0] - va->position[0],
        vc->position[1] - va->position[1],
        vc->position[2] - va->position[2],
    };
    float geom[3] = {
        e1[1] * e2[2] - e1[2] * e2[1],
        e1[2] * e2[0] - e1[0] * e2[2],
        e1[0] * e2[1] - e1[1] * e2[0],
    };
    float normal[3] = {
        va->normal[0] + vb->normal[0] + vc->normal[0],
        va->normal[1] + vb->normal[1] + vc->normal[1],
        va->normal[2] + vb->normal[2] + vc->normal[2],
    };

    float geom_len2 = dot3(geom, geom);
    float normal_len2 = dot3(normal, normal);
    if (geom_len2 <= 1.0e-12f || normal_len2 <= 1.0e-8f) {
        return 0;
    }

    float d = dot3(geom, normal);
    return d < 0.0f && (d * d) > (geom_len2 * normal_len2 * 1.0e-4f);
}

static int verify_gp_ir_triangle_winding(const ThreediModelIR *ir, const char *path) {
    for (size_t lod_idx = 0; lod_idx < ir->lod_count; ++lod_idx) {
        const ThreediIRLod *lod = &ir->lods[lod_idx];
        for (size_t prim_idx = 0; prim_idx < lod->primitive_count; ++prim_idx) {
            const ThreediIRPrimitive *prim = &lod->primitives[prim_idx];
            if (prim->topology != THREEDI_IR_TOPOLOGY_TRIANGLES) {
                fprintf(stderr, "GP IR primitive is not triangulated in %s\n", path);
                return 0;
            }
            for (uint32_t i = 0; i + 2 < prim->index_count; i += 3) {
                size_t idx = (size_t)prim->index_offset + i;
                if (idx + 2 >= lod->index_count) {
                    fprintf(stderr, "Out-of-range GP IR index span in %s\n", path);
                    return 0;
                }
                if (ir_triangle_opposes_authored_normals(
                        lod,
                        prim->vertex_offset,
                        lod->indices[idx + 0],
                        lod->indices[idx + 1],
                        lod->indices[idx + 2])) {
                    fprintf(stderr,
                            "Reversed GP IR triangle in %s (lod=%zu prim=%zu tri=%u)\n",
                            path, lod_idx, prim_idx, i / 3);
                    return 0;
                }
            }
        }
    }
    return 1;
}

static int test_gp_parse(const char *path) {
    ThreediGpFile gp;
    threedi_gp_init(&gp);

    if (threedi_gp_read(path, &gp) != 0) {
        fprintf(stderr, "Failed to parse %s\n", path);
        return 0;
    }

    // Basic sanity checks
    if (gp.header.mesh_type == THREEDI_GP_MESH_UNKNOWN) {
        fprintf(stderr, "Unknown mesh type for %s\n", path);
        threedi_gp_free(&gp);
        return 0;
    }

    if (gp.header.num_lods <= 0 || gp.header.num_lods > 4) {
        fprintf(stderr, "Invalid LOD count %d for %s\n", gp.header.num_lods, path);
        threedi_gp_free(&gp);
        return 0;
    }

    if (gp.rvert_count == 0) {
        fprintf(stderr, "No vertices for %s\n", path);
        threedi_gp_free(&gp);
        return 0;
    }

    // Verify collision object sizes if present
    if (gp.collision && gp.collision->object_count > 0) {
        for (int32_t i = 0; i < gp.collision->object_count; ++i) {
            ThreediGpCollisionObject *obj = &gp.collision->objects[i];
            // Each collision object should have reasonable counts
            if (obj->vertex_count < 0 || obj->face_count < 0 || obj->volume_count < 0) {
                fprintf(stderr, "Invalid collision object counts in %s\n", path);
                threedi_gp_free(&gp);
                return 0;
            }
        }
    }

    // Verify occlusion object sizes if present
    if (gp.occlusion && gp.occlusion->object_count > 0) {
        for (size_t i = 0; i < gp.occlusion->object_count; ++i) {
            ThreediGpOcclusionObject *obj = &gp.occlusion->objects[i];
            // Each occlusion object should have reasonable counts
            if (obj->num_vertices < 0 || obj->num_planes < 0 || obj->num_faces < 0) {
                fprintf(stderr, "Invalid occlusion object counts in %s\n", path);
                threedi_gp_free(&gp);
                return 0;
            }
        }
    }

    // Test IR conversion
    ThreediModelIR ir;
    threedi_ir_init(&ir);
    if (threedi_ir_from_gp(&gp, &ir) != 0) {
        fprintf(stderr, "Failed to convert %s to IR\n", path);
        threedi_gp_free(&gp);
        return 0;
    }

    // Verify IR has expected data
    if (ir.lod_count == 0) {
        fprintf(stderr, "No LODs in IR for %s\n", path);
        threedi_ir_free(&ir);
        threedi_gp_free(&gp);
        return 0;
    }

    if (!verify_gp_ir_triangle_winding(&ir, path)) {
        threedi_ir_free(&ir);
        threedi_gp_free(&gp);
        return 0;
    }

    threedi_ir_free(&ir);
    threedi_gp_free(&gp);
    return 1;
}

int main(void) {
    const char *repo_root = test_paths_repo_root(__FILE__);
    char fixtures_dir[4096];
    char **files = NULL;
    size_t file_count = 0, file_cap = 0;
    size_t i;

    snprintf(fixtures_dir, sizeof(fixtures_dir), "%s/fixtures/threedi/gp", repo_root);

    DIR *d = opendir(fixtures_dir);
    if (!d) {
        fprintf(stderr, "No GP fixtures found at %s\n", fixtures_dir);
        return EXIT_FAILURE;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!has_extension(ent->d_name, ".3di")) continue;

        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", fixtures_dir, ent->d_name);

        // Only test GP format files
        if (!is_gp_file(path)) continue;

        if (file_count >= file_cap) {
            file_cap = file_cap ? file_cap * 2 : 64;
            files = (char **)realloc(files, file_cap * sizeof(char *));
        }
        files[file_count] = (char *)malloc(strlen(path) + 1);
        strcpy(files[file_count], path);
        file_count++;
    }
    closedir(d);

    if (file_count == 0) {
        fprintf(stderr, "No GP format .3di files found in %s\n", fixtures_dir);
        free(files);
        return EXIT_FAILURE;
    }

    printf("Testing %zu GP format files...\n", file_count);

    size_t passed = 0, failed = 0;
    for (i = 0; i < file_count; ++i) {
        const char *name = strrchr(files[i], '/');
        name = name ? name + 1 : files[i];
        printf("  %s... ", name);
        fflush(stdout);

        if (test_gp_parse(files[i])) {
            printf("OK\n");
            passed++;
        } else {
            printf("FAILED\n");
            failed++;
        }
    }

    for (i = 0; i < file_count; ++i) free(files[i]);
    free(files);

    if (failed > 0) {
        printf("GP parse test: %zu passed, %zu failed\n", passed, failed);
        return EXIT_FAILURE;
    }

    printf("All %zu GP parse tests passed.\n", passed);
    return EXIT_SUCCESS;
}
