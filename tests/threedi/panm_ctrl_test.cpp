// PANM/CTRL byte stability through the parity writer: the chunk bytes of the
// synthetic register-controlled models (fixtures/threedi/synth) must survive
// a read -> write round trip unchanged, and, when OPENNOVA_JO_ASSETS names an
// extracted retail tree, the six retail controlled models it carries
// (docs/asset-gated-tests.md).
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <formats/threedi/threedi.h>
#include <formats/threedi/threedi_3di3.h>
#include "common/retail_paths.h"
#include "common/test_paths.h"

static const ThreediChunk *find_first_chunk(const ThreediChunk *chunk,
                                             const char id[4]) {
    size_t i;
    if (!chunk) return NULL;
    if (memcmp(chunk->id, id, 4) == 0) return chunk;
    for (i = 0; i < chunk->child_count; ++i) {
        const ThreediChunk *found = find_first_chunk(&chunk->children[i], id);
        if (found) return found;
    }
    return NULL;
}

typedef struct {
    int found;
    unsigned char *data;
    size_t data_len;
} ChunkBytes;

static ChunkBytes load_chunk_bytes(const char *path, const char id[4]) {
    ChunkBytes out;
    ThreediFile file;
    const ThreediChunk *chunk;

    memset(&out, 0, sizeof(out));
    memset(&file, 0, sizeof(file));

    if (threedi_read_file(path, &file) != 0) {
        fprintf(stderr, "failed to read %s\n", path);
        return out;
    }
    chunk = find_first_chunk(file.root, id);
    if (chunk) {
        out.found = 1;
        out.data_len = chunk->data_len;
        out.data = (unsigned char *)malloc(chunk->data_len);
        if (out.data) {
            memcpy(out.data, chunk->data, chunk->data_len);
        }
    }
    threedi_free_file(&file);
    return out;
}

static int roundtrip_and_compare(const char *path, const char id[4]) {
    ChunkBytes before, after;
    Threedi3di3 model;
    char tmp[4096];
    int ok = 1;

    before = load_chunk_bytes(path, id);
    if (!before.found) {
        fprintf(stderr, "%s missing %c%c%c%c\n", path,
                id[0], id[1], id[2], id[3]);
        return 0;
    }

    memset(&model, 0, sizeof(model));
    if (threedi_3di3_read(path, &model) != 0) {
        fprintf(stderr, "threedi_3di3_read failed for %s\n", path);
        free(before.data);
        return 0;
    }
    const char *base = strrchr(path, '/');
#ifdef _WIN32
    { const char *bs = strrchr(path, '\\'); if (bs && (!base || bs > base)) base = bs; }
#endif
    base = base ? base + 1 : path;
    snprintf(tmp, sizeof(tmp), "%s/%s.rt", test_paths_temp_dir(), base);
    if (threedi_3di3_write(tmp, &model) != 0) {
        fprintf(stderr, "threedi_3di3_write failed for %s\n", tmp);
        ok = 0;
    } else {
        after = load_chunk_bytes(tmp, id);
        if (before.found != after.found) {
            fprintf(stderr, "%c%c%c%c presence mismatch after roundtrip\n",
                    id[0], id[1], id[2], id[3]);
            ok = 0;
        } else if (before.data_len != after.data_len ||
                   memcmp(before.data, after.data, before.data_len) != 0) {
            fprintf(stderr, "%c%c%c%c bytes changed after roundtrip\n",
                    id[0], id[1], id[2], id[3]);
            ok = 0;
        }
        free(after.data);
    }
    threedi_3di3_free(&model);
    remove(tmp);
    free(before.data);
    return ok;
}

// The synthetic set: every model that authors CTRL registers and
// register-driven PANM rows (tests/fixtures/minimal_3di_gen.cpp).
static int check_synthetic_leg(const char *repo_root) {
    static const char *kSynthetic[] = {"mount", "tank", "carrier", "pump", "armory"};
    char path[4096];
    int ok = 1;
    for (size_t i = 0; i < sizeof(kSynthetic) / sizeof(kSynthetic[0]); ++i) {
        snprintf(path, sizeof(path), "%s/fixtures/threedi/synth/%s.3di", repo_root, kSynthetic[i]);
        ok &= roundtrip_and_compare(path, "PANM");
        ok &= roundtrip_and_compare(path, "CTRL");
    }
    return ok;
}

// The retail leg: the six controlled models of the JO corpus, byte-stable
// through the same writer.
static int check_retail_leg(void) {
    static const char *kRetail[] = {"B50Cal.3di", "CarierU.3di", "DLCAC2.3di",
                                    "dm1a1.3di", "dsuv1.3di", "M1trret.3di"};
    int ok = 1;
    for (size_t i = 0; i < sizeof(kRetail) / sizeof(kRetail[0]); ++i) {
        const std::string path = retail::asset_file(kRetail[i]);
        if (path.empty()) {
            // Not every extract carries every controlled model (CarierU and
            // DLCAC2 ship in expansion packs): an absent file skips its leg.
            char needs[96];
            snprintf(needs, sizeof(needs), "%s under OPENNOVA_JO_ASSETS", kRetail[i]);
            retail::skip_leg(needs);
            continue;
        }
        ok &= roundtrip_and_compare(path.c_str(), "PANM");
        ok &= roundtrip_and_compare(path.c_str(), "CTRL");
    }
    return ok;
}

int main(void) {
    const char *repo_root = test_paths_repo_root(__FILE__);
    int ok = check_synthetic_leg(repo_root);
    if (retail::assets().empty()) {
        retail::skip_leg("OPENNOVA_JO_ASSETS for the six retail controlled models");
    } else {
        ok &= check_retail_leg();
    }
    return ok ? 0 : 1;
}
