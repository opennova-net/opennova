// GP format roundtrip test - verifies byte-for-byte roundtrip of GP files.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/dirent_compat.h"
#include <sys/stat.h>

#include "threedi/threedi_gp.h"
#include "common/test_paths.h"

static int has_extension(const char *name, const char *ext) {
    size_t nlen = strlen(name);
    size_t elen = strlen(ext);
    if (nlen < elen) return 0;
    return strcmp(name + nlen - elen, ext) == 0;
}

static int roundtrip(const char *path) {
    ThreediGpFile gp;
    char tmp[4096];
    FILE *fa, *fb;
    long orig_size, round_size;
    unsigned char *bufA, *bufB;
    int same;

    threedi_gp_init(&gp);
    if (threedi_gp_read(path, &gp) != 0) {
        fprintf(stderr, "Failed to read %s\n", path);
        return 0;
    }

    snprintf(tmp, sizeof(tmp), "%s.roundtrip", path);
    if (threedi_gp_write(tmp, &gp) != 0) {
        threedi_gp_free(&gp);
        fprintf(stderr, "Failed to write roundtrip %s\n", tmp);
        return 0;
    }
    threedi_gp_free(&gp);

    // Compare byte-for-byte
    fa = fopen(path, "rb");
    fb = fopen(tmp, "rb");
    if (!fa || !fb) {
        if (fa) fclose(fa);
        if (fb) fclose(fb);
        remove(tmp);
        fprintf(stderr, "Failed to open files for comparison\n");
        return 0;
    }

    fseek(fa, 0, SEEK_END); orig_size = ftell(fa); fseek(fa, 0, SEEK_SET);
    fseek(fb, 0, SEEK_END); round_size = ftell(fb); fseek(fb, 0, SEEK_SET);

    if (orig_size != round_size) {
        fprintf(stderr, "Size mismatch for %s: orig=%ld, roundtrip=%ld (diff=%ld)\n",
                path, orig_size, round_size, round_size - orig_size);
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

    if (!same) {
        // Find first difference
        for (long i = 0; i < orig_size; ++i) {
            if (bufA[i] != bufB[i]) {
                fprintf(stderr, "Roundtrip mismatch for %s at offset 0x%lx: orig=0x%02x, roundtrip=0x%02x\n",
                        path, i, bufA[i], bufB[i]);
                break;
            }
        }
    }

    free(bufA);
    free(bufB);
    remove(tmp);

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

    snprintf(fixtures_dir, sizeof(fixtures_dir), "%s/fixtures/threedi/gp", repo_root);

    d = opendir(fixtures_dir);
    if (!d) {
        fprintf(stderr, "No GP fixture files found under %s\n", fixtures_dir);
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
        fprintf(stderr, "No GP fixture .3di files found under %s\n", fixtures_dir);
        free(files);
        return EXIT_FAILURE;
    }

    printf("Testing %zu GP files for roundtrip...\n", file_count);

    size_t passed = 0, failed = 0;
    for (i = 0; i < file_count; ++i) {
        const char *name = strrchr(files[i], '/');
        name = name ? name + 1 : files[i];
        printf("  %s... ", name);
        fflush(stdout);

        if (roundtrip(files[i])) {
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
        printf("GP roundtrip test: %zu passed, %zu failed\n", passed, failed);
        return EXIT_FAILURE;
    }

    printf("All %zu GP roundtrip tests passed.\n", passed);
    return EXIT_SUCCESS;
}
