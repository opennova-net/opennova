// Roundtrip all reference 3DIs through the threedi parser/writer.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/dirent_compat.h"
#include <sys/stat.h>

#include "threedi/threedi_3di3.h"
#include "common/test_paths.h"

static int has_extension(const char *name, const char *ext) {
    size_t nlen = strlen(name);
    size_t elen = strlen(ext);
    if (nlen < elen) return 0;
    return strcmp(name + nlen - elen, ext) == 0;
}

static int roundtrip(const char *path) {
    Threedi3di3 model;
    char tmp[4096];
    FILE *fa, *fb;
    long orig_size, round_size;
    unsigned char *bufA, *bufB;
    int same;

    memset(&model, 0, sizeof(model));
    if (threedi_3di3_read(path, &model) != 0) {
        fprintf(stderr, "Failed to read %s\n", path);
        return 0;
    }
    snprintf(tmp, sizeof(tmp), "%s.roundtrip", path);
    if (threedi_3di3_write(tmp, &model) != 0) {
        threedi_3di3_free(&model);
        fprintf(stderr, "Failed to write roundtrip %s\n", tmp);
        return 0;
    }
    threedi_3di3_free(&model);

    // Compare byte-for-byte
    fa = fopen(path, "rb");
    fb = fopen(tmp, "rb");
    if (!fa || !fb) {
        if (fa) fclose(fa);
        if (fb) fclose(fb);
        remove(tmp);
        return 0;
    }
    fseek(fa, 0, SEEK_END); orig_size = ftell(fa); fseek(fa, 0, SEEK_SET);
    fseek(fb, 0, SEEK_END); round_size = ftell(fb); fseek(fb, 0, SEEK_SET);
    if (orig_size != round_size) {
        fprintf(stderr, "Size mismatch for %s\n", path);
        fclose(fa); fclose(fb);
        remove(tmp);
        return 0;
    }
    bufA = (unsigned char *)malloc((size_t)orig_size);
    bufB = (unsigned char *)malloc((size_t)round_size);
    if (!bufA || !bufB) {
        free(bufA); free(bufB);
        fclose(fa); fclose(fb);
        remove(tmp);
        return 0;
    }
    fread(bufA, 1, (size_t)orig_size, fa);
    fread(bufB, 1, (size_t)round_size, fb);
    fclose(fa);
    fclose(fb);

    same = (memcmp(bufA, bufB, (size_t)orig_size) == 0);
    free(bufA);
    free(bufB);
    remove(tmp);

    if (!same) {
        fprintf(stderr, "Roundtrip mismatch for %s\n", path);
    }
    return same;
}

int main(void) {
    const char *repo_root = test_paths_repo_root(__FILE__);
    char fixtures_dir[4096];
    DIR *d;
    struct dirent *ent;
    char **files = NULL;
    size_t file_count = 0, file_cap = 0;
    size_t i;

    snprintf(fixtures_dir, sizeof(fixtures_dir), "%s/fixtures/threedi/3di3", repo_root);

    d = opendir(fixtures_dir);
    if (!d) {
        fprintf(stderr, "No fixture 3di files found under %s\n", fixtures_dir);
        return EXIT_FAILURE;
    }
    while ((ent = readdir(d)) != NULL) {
        if (has_extension(ent->d_name, ".3di")) {
            char path[4096];
            snprintf(path, sizeof(path), "%s/%s", fixtures_dir, ent->d_name);
            if (file_count >= file_cap) {
                file_cap = file_cap ? file_cap * 2 : 16;
                files = (char **)realloc(files, file_cap * sizeof(char *));
            }
            files[file_count] = (char *)malloc(strlen(path) + 1);
            strcpy(files[file_count], path);
            file_count++;
        }
    }
    closedir(d);

    if (file_count == 0) {
        fprintf(stderr, "No fixture 3di files found under %s\n", fixtures_dir);
        free(files);
        return EXIT_FAILURE;
    }

    printf("Testing %zu 3DI files...\n", file_count);
    for (i = 0; i < file_count; ++i) {
        // Extract filename for display
        const char *name = strrchr(files[i], '/');
        name = name ? name + 1 : files[i];
        printf("  %s... ", name);
        if (!roundtrip(files[i])) {
            // Cleanup
            for (i = 0; i < file_count; ++i) free(files[i]);
            free(files);
            return EXIT_FAILURE;
        }
        printf("OK\n");
    }
    printf("All roundtrip tests passed.\n");

    for (i = 0; i < file_count; ++i) free(files[i]);
    free(files);
    return EXIT_SUCCESS;
}
