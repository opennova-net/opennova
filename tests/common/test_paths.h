// Pure C helper to locate the repo root directory from __FILE__.
// Walks parent directories looking for both "fixtures/" and "libs/".

#ifndef TEST_PATHS_H
#define TEST_PATHS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define TEST_PATHS_GETCWD _getcwd
#define TEST_PATHS_SEP '\\'
#else
#include <unistd.h>
#define TEST_PATHS_GETCWD getcwd
#define TEST_PATHS_SEP '/'
#endif

static int test_paths_dir_exists(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
#ifdef _WIN32
    return (st.st_mode & _S_IFDIR) != 0;
#else
    return S_ISDIR(st.st_mode);
#endif
}

static int test_paths_is_repo_root(const char *dir) {
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s%cfixtures", dir, TEST_PATHS_SEP);
    if (!test_paths_dir_exists(buf)) return 0;
    snprintf(buf, sizeof(buf), "%s%clibs", dir, TEST_PATHS_SEP);
    if (!test_paths_dir_exists(buf)) return 0;
    return 1;
}

// Returns a pointer to a static buffer containing the repo root path.
// Searches upward from the directory containing `file_path` (__FILE__).
static const char *test_paths_repo_root(const char *file_path) {
    static char result[4096];
    char candidate[4096];
    char *last_sep;

    // Start from the directory containing file_path
    strncpy(candidate, file_path, sizeof(candidate) - 1);
    candidate[sizeof(candidate) - 1] = '\0';

    // Strip filename to get directory
    last_sep = strrchr(candidate, '/');
#ifdef _WIN32
    {
        char *bs = strrchr(candidate, '\\');
        if (bs && (!last_sep || bs > last_sep)) last_sep = bs;
    }
#endif
    if (last_sep) {
        *last_sep = '\0';
    }

    // Walk upward
    for (;;) {
        if (test_paths_is_repo_root(candidate)) {
            strncpy(result, candidate, sizeof(result) - 1);
            result[sizeof(result) - 1] = '\0';
            return result;
        }
        last_sep = strrchr(candidate, '/');
#ifdef _WIN32
        {
            char *bs = strrchr(candidate, '\\');
            if (bs && (!last_sep || bs > last_sep)) last_sep = bs;
        }
#endif
        if (!last_sep) break;
        *last_sep = '\0';
    }

    // Fallback: try CWD
    if (TEST_PATHS_GETCWD(candidate, sizeof(candidate))) {
        while (candidate[0]) {
            if (test_paths_is_repo_root(candidate)) {
                strncpy(result, candidate, sizeof(result) - 1);
                result[sizeof(result) - 1] = '\0';
                return result;
            }
            last_sep = strrchr(candidate, '/');
#ifdef _WIN32
            {
                char *bs = strrchr(candidate, '\\');
                if (bs && (!last_sep || bs > last_sep)) last_sep = bs;
            }
#endif
            if (!last_sep) break;
            *last_sep = '\0';
        }
    }

    // Last resort: return parent of file_path
    strncpy(result, file_path, sizeof(result) - 1);
    result[sizeof(result) - 1] = '\0';
    last_sep = strrchr(result, '/');
#ifdef _WIN32
    {
        char *bs = strrchr(result, '\\');
        if (bs && (!last_sep || bs > last_sep)) last_sep = bs;
    }
#endif
    if (last_sep) *last_sep = '\0';
    return result;
}

static const char *test_paths_temp_dir(void) {
#ifdef _WIN32
    const char *tmp = getenv("TEMP");
    if (!tmp) tmp = getenv("TMP");
    if (!tmp) tmp = ".";
    return tmp;
#else
    return "/tmp";
#endif
}

#endif // TEST_PATHS_H
