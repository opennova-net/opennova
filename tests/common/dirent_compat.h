// Minimal dirent compatibility for MSVC.
// On non-Windows platforms this just includes <dirent.h>.

#ifndef DIRENT_COMPAT_H
#define DIRENT_COMPAT_H

#ifdef _WIN32

#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif

struct dirent {
    char d_name[MAX_PATH];
};

typedef struct {
    HANDLE handle;
    WIN32_FIND_DATAA data;
    struct dirent ent;
    int first;
} DIR;

static DIR *opendir(const char *path) {
    char pattern[MAX_PATH];
    DIR *d = (DIR *)malloc(sizeof(DIR));
    if (!d) return NULL;

    snprintf(pattern, MAX_PATH, "%s\\*", path);
    d->handle = FindFirstFileA(pattern, &d->data);
    if (d->handle == INVALID_HANDLE_VALUE) {
        free(d);
        return NULL;
    }
    d->first = 1;
    return d;
}

static struct dirent *readdir(DIR *d) {
    if (d->first) {
        d->first = 0;
    } else {
        if (!FindNextFileA(d->handle, &d->data))
            return NULL;
    }
    strncpy(d->ent.d_name, d->data.cFileName, MAX_PATH - 1);
    d->ent.d_name[MAX_PATH - 1] = '\0';
    return &d->ent;
}

static void closedir(DIR *d) {
    if (d) {
        FindClose(d->handle);
        free(d);
    }
}

#else
#include <dirent.h>
#endif

#endif // DIRENT_COMPAT_H
