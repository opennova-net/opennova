// Test: load a .3di fixture, compute a .3dp via tdp_from_ir, compare to expected .3dp.
// Iterates all subdirectories in fixtures/3dp/ so new fixtures are picked up automatically.

#include "tdp/tdp.h"
#include "threedi/threedi_ir.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

#include "common/dirent_compat.h"
#include "common/test_paths.h"

// Strip trailing whitespace (including \r\n) and return length.
static size_t strip_trailing(char *line) {
    size_t len = std::strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                       line[len - 1] == ' '  || line[len - 1] == '\t'))
        line[--len] = '\0';
    return len;
}

// Check if line is a material name line: `    name            "..."`
static bool is_material_name_line(const char *line) {
    // Skip leading whitespace
    const char *p = line;
    while (*p == ' ' || *p == '\t') ++p;
    return std::strncmp(p, "name", 4) == 0 &&
           (p[4] == ' ' || p[4] == '\t');
}

// Check if line is an axis func line (contains _func)
static bool is_func_line(const char *line) {
    return std::strstr(line, "_func") != nullptr;
}

// Check if line is a reflect_rgb line (genuinely lost for non-glass shaders)
static bool is_reflect_rgb_line(const char *line) {
    const char *p = line;
    while (*p == ' ' || *p == '\t') ++p;
    return std::strncmp(p, "reflect_rgb", 11) == 0 &&
           (p[11] == ' ' || p[11] == '\t');
}

// Check if line is a texture line (diffusetex[N] or normaltex[N])
static bool is_texture_line(const char *line) {
    const char *p = line;
    while (*p == ' ' || *p == '\t') ++p;
    return std::strncmp(p, "diffusetex[", 11) == 0 ||
           std::strncmp(p, "normaltex[", 10) == 0;
}

// Normalize "0" to "" for texture name comparison
static bool texture_lines_match(const char *a, const char *b) {
    auto extract_name = [](const char *line, char *out, size_t out_sz) {
        const char *q1 = std::strchr(line, '"');
        if (!q1) { out[0] = '\0'; return; }
        const char *q2 = std::strchr(q1 + 1, '"');
        if (!q2) { out[0] = '\0'; return; }
        size_t len = (size_t)(q2 - q1 - 1);
        if (len >= out_sz) len = out_sz - 1;
        std::memcpy(out, q1 + 1, len);
        out[len] = '\0';
    };
    auto normalize = [](const char *s) -> const char * {
        return (s[0] == '0' && s[1] == '\0') ? "" : s;
    };
    char na[256], nb[256];
    extract_name(a, na, sizeof(na));
    extract_name(b, nb, sizeof(nb));
    return std::strcmp(normalize(na), normalize(nb)) == 0;
}

// Parse a float token starting at *pp, advance *pp past it.
// Returns true on success.
static bool parse_float(const char **pp, double *out) {
    const char *p = *pp;
    while (*p == ' ' || *p == '\t') ++p;
    char *end = nullptr;
    *out = std::strtod(p, &end);
    if (end == p) return false;
    *pp = end;
    return true;
}

// Parse an int token starting at *pp.
static bool parse_int(const char **pp, int *out) {
    const char *p = *pp;
    while (*p == ' ' || *p == '\t') ++p;
    char *end = nullptr;
    long v = std::strtol(p, &end, 10);
    if (end == p) return false;
    *out = static_cast<int>(v);
    *pp = end;
    return true;
}

// Compare two _func lines with fuzzy float tolerance.
// Format: "            yaw_func         0  0.000 0.000 0.000 0.000 CTRL_NAME"
// The label and func_id should match exactly, floats within tolerance, ctrl_reg exact.
static bool func_lines_match(const char *actual, const char *expected) {
    // Find _func in both
    const char *fa = std::strstr(actual, "_func");
    const char *fb = std::strstr(expected, "_func");
    if (!fa || !fb) return false;

    // Compare everything up through the label (prefix + label name)
    // Find start of label (first non-space before _func)
    const char *la = fa;
    while (la > actual && *(la - 1) != ' ' && *(la - 1) != '\t') --la;
    const char *lb = fb;
    while (lb > expected && *(lb - 1) != ' ' && *(lb - 1) != '\t') --lb;

    // Compare prefix (indentation) length
    size_t prefix_a = static_cast<size_t>(la - actual);
    size_t prefix_b = static_cast<size_t>(lb - expected);
    if (prefix_a != prefix_b) return false;
    if (std::memcmp(actual, expected, prefix_a) != 0) return false;

    // Find end of "_func" + trailing spaces to get to the values
    const char *pa = std::strchr(fa, ' ');
    const char *pb = std::strchr(fb, ' ');
    if (!pa || !pb) return false;

    // Compare the label itself
    size_t label_a = static_cast<size_t>(pa - la);
    size_t label_b = static_cast<size_t>(pb - lb);
    if (label_a != label_b) return false;
    if (std::memcmp(la, lb, label_a) != 0) return false;

    // Parse func_id (integer)
    int id_a, id_b;
    if (!parse_int(&pa, &id_a) || !parse_int(&pb, &id_b)) return false;
    if (id_a != id_b) return false;

    // Parse float values with tolerance.
    // For register-based functions (func > 112), param1 (4th float) is not
    // preserved in the 3DI binary — the binary stores a register index in
    // control_param instead of the original phase value. So we compare only
    // the first 3 floats for register functions.
    int float_count = (id_a > 112) ? 3 : 4;
    for (int i = 0; i < float_count; ++i) {
        double va, vb;
        if (!parse_float(&pa, &va) || !parse_float(&pb, &vb)) return false;
        // Rotation uses 16384/360 encoding (~0.022 degree precision)
        // Other transforms use *256 encoding (~0.004 precision)
        // Use 0.025 tolerance to cover both
        if (std::fabs(va - vb) > 0.025) return false;
    }

    // ctrlReg on non-register functions (func <= 112) cannot survive the
    // binary roundtrip — the engine only resolves register names for func 113+.
    if (id_a <= 112) return true;

    // Skip param1 in expected (we already skipped it in actual above)
    { double skip; parse_float(&pa, &skip); parse_float(&pb, &skip); }

    // Rest is ctrl_reg name — compare exactly (after trimming whitespace)
    while (*pa == ' ' || *pa == '\t') ++pa;
    while (*pb == ' ' || *pb == '\t') ++pb;
    if (std::strcmp(pa, pb) != 0) return false;

    return true;
}

// Compare two text files line-by-line with special handling:
// - Ignores line-ending and trailing whitespace differences
// - Skips material name lines (not recoverable from binary)
// - Uses fuzzy float comparison for _func lines (int16 precision loss)
// Returns 0 if identical, prints first difference and returns -1.
static int compare_files(const char *actual_path, const char *expected_path) {
    FILE *fa = std::fopen(actual_path, "r");
    FILE *fb = std::fopen(expected_path, "r");
    if (!fa) {
        std::fprintf(stderr, "Cannot open actual: %s\n", actual_path);
        if (fb) std::fclose(fb);
        return -1;
    }
    if (!fb) {
        std::fprintf(stderr, "Cannot open expected: %s\n", expected_path);
        std::fclose(fa);
        return -1;
    }

    char line_a[2048], line_b[2048];
    int lineno = 0;
    int result = 0;

    while (true) {
        bool got_a = std::fgets(line_a, sizeof(line_a), fa) != nullptr;
        bool got_b = std::fgets(line_b, sizeof(line_b), fb) != nullptr;
        lineno++;

        if (!got_a && !got_b) break; // Both EOF — match.

        if (!got_a) {
            std::fprintf(stderr, "  Actual file ended early at line %d\n", lineno);
            strip_trailing(line_b);
            std::fprintf(stderr, "  Expected: [%s]\n", line_b);
            result = -1;
            break;
        }
        if (!got_b) {
            std::fprintf(stderr, "  Expected file ended early at line %d\n", lineno);
            strip_trailing(line_a);
            std::fprintf(stderr, "  Actual:   [%s]\n", line_a);
            result = -1;
            break;
        }

        strip_trailing(line_a);
        strip_trailing(line_b);

        // Skip material name lines (not recoverable from binary data)
        if (is_material_name_line(line_a) && is_material_name_line(line_b))
            continue;

        // Skip reflect_rgb lines (only preserved for glass shaders with flag 0x2000)
        if (is_reflect_rgb_line(line_a) && is_reflect_rgb_line(line_b))
            continue;

        // Texture lines: treat "0" and "" as equivalent (convention difference)
        if (is_texture_line(line_a) && is_texture_line(line_b)) {
            if (texture_lines_match(line_a, line_b))
                continue;
        }

        // Fuzzy comparison for _func lines (int16 encoding precision loss)
        if (is_func_line(line_a) && is_func_line(line_b)) {
            if (func_lines_match(line_a, line_b))
                continue;
        }

        if (std::strcmp(line_a, line_b) != 0) {
            std::fprintf(stderr, "  Mismatch at line %d:\n", lineno);
            std::fprintf(stderr, "  Actual:   [%s]\n", line_a);
            std::fprintf(stderr, "  Expected: [%s]\n", line_b);
            result = -1;
            break;
        }
    }

    std::fclose(fa);
    std::fclose(fb);
    return result;
}

static int test_fixture(const char *fixture_dir, const char *name) {
    char path_3di[4096], path_3dp_expected[4096], path_3dp_actual[4096];

    std::snprintf(path_3di, sizeof(path_3di), "%s/%s.3di", fixture_dir, name);
    std::snprintf(path_3dp_expected, sizeof(path_3dp_expected), "%s/%s.3dp", fixture_dir, name);
    std::snprintf(path_3dp_actual, sizeof(path_3dp_actual), "%s/tdp_from_3di_test_%s.3dp",
                  test_paths_temp_dir(), name);

    // Load 3DI → IR
    ThreediModelIR ir;
    threedi_ir_init(&ir);
    int rc = threedi_ir_read(path_3di, &ir);
    if (rc != 0) {
        std::fprintf(stderr, "FAIL [%s]: threedi_ir_read failed for %s\n", name, path_3di);
        return -1;
    }

    // IR → TdpProject
    TdpProject proj;
    rc = tdp_from_ir(&ir, &proj);
    if (rc != 0) {
        std::fprintf(stderr, "FAIL [%s]: tdp_from_ir failed\n", name);
        threedi_ir_free(&ir);
        return -1;
    }

    // Write to temp file
    rc = tdp_write(path_3dp_actual, &proj);
    if (rc != 0) {
        std::fprintf(stderr, "FAIL [%s]: tdp_write failed\n", name);
        tdp_free(&proj);
        threedi_ir_free(&ir);
        return -1;
    }

    // Compare
    rc = compare_files(path_3dp_actual, path_3dp_expected);
    if (rc != 0) {
        std::fprintf(stderr, "FAIL [%s]: output differs from expected\n", name);
        std::fprintf(stderr, "  Actual:   %s\n", path_3dp_actual);
        std::fprintf(stderr, "  Expected: %s\n", path_3dp_expected);
        // Leave the actual file for debugging
    } else {
        // Clean up on success
        std::remove(path_3dp_actual);
    }

    tdp_free(&proj);
    threedi_ir_free(&ir);
    return rc;
}

int main() {
    const char *root = test_paths_repo_root(__FILE__);
    char fixtures_dir[4096];
    std::snprintf(fixtures_dir, sizeof(fixtures_dir), "%s/fixtures/3dp", root);

    DIR *dp = opendir(fixtures_dir);
    if (!dp) {
        std::fprintf(stderr, "Cannot open fixtures directory: %s\n", fixtures_dir);
        return 1;
    }

    int total = 0, passed = 0, failed = 0;
    struct dirent *ent;
    while ((ent = readdir(dp)) != nullptr) {
        // Skip . and ..
        if (ent->d_name[0] == '.') continue;

        // Check that it's a directory containing a .3di and .3dp
        char subdir[4096], check_3di[4096], check_3dp[4096];
        std::snprintf(subdir, sizeof(subdir), "%s/%s", fixtures_dir, ent->d_name);

        struct stat st;
        if (stat(subdir, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        std::snprintf(check_3di, sizeof(check_3di), "%s/%s.3di", subdir, ent->d_name);
        std::snprintf(check_3dp, sizeof(check_3dp), "%s/%s.3dp", subdir, ent->d_name);

        struct stat st_3di, st_3dp;
        if (stat(check_3di, &st_3di) != 0 || stat(check_3dp, &st_3dp) != 0) continue;

        total++;
        std::printf("Testing %s ... ", ent->d_name);
        std::fflush(stdout);

        if (test_fixture(subdir, ent->d_name) == 0) {
            std::printf("PASSED\n");
            passed++;
        } else {
            std::printf("FAILED\n");
            failed++;
        }
    }
    closedir(dp);

    if (total == 0) {
        std::fprintf(stderr, "No fixtures found in %s\n", fixtures_dir);
        return 1;
    }

    std::printf("\n%d/%d fixtures passed", passed, total);
    if (failed > 0) std::printf(", %d FAILED", failed);
    std::printf("\n");

    return failed > 0 ? 1 : 0;
}
