#include "threedi/threedi.h"

#include "threedi/threedi_3di3.h" // THREEDI_3DI3_PARENT_FLAG / _LENGTH_MASK

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static int read_u32(FILE *f, uint32_t *out)
{
    unsigned char buf[4];
    if (fread(buf, 1, 4, f) != 4) {
        return -1;
    }
    *out = (uint32_t)buf[0]
         | ((uint32_t)buf[1] << 8)
         | ((uint32_t)buf[2] << 16)
         | ((uint32_t)buf[3] << 24);
    return 0;
}

static void free_chunk(ThreediChunk *chunk)
{
    if (!chunk) {
        return;
    }
    for (size_t i = 0; i < chunk->child_count; ++i) {
        free_chunk(&chunk->children[i]);
    }
    free(chunk->children);
}

static int parse_chunk(const uint8_t *buf, size_t buf_len, size_t cursor, ThreediChunk *out_chunk, size_t *advance)
{
    memset(out_chunk, 0, sizeof(*out_chunk));
    if (cursor + 8 > buf_len) {
        return -1;
    }

    memcpy(out_chunk->id, buf + cursor, 4);
    out_chunk->id[4] = '\0';
    uint32_t len_flags = (uint32_t)buf[cursor + 4]
                       | ((uint32_t)buf[cursor + 5] << 8)
                       | ((uint32_t)buf[cursor + 6] << 16)
                       | ((uint32_t)buf[cursor + 7] << 24);

    out_chunk->is_parent = (len_flags & THREEDI_3DI3_PARENT_FLAG) != 0;
    out_chunk->content_len = len_flags & THREEDI_3DI3_LENGTH_MASK;
    out_chunk->offset = cursor;

    size_t content_start = cursor + 8;
    size_t content_end = content_start + out_chunk->content_len;
    if (content_end > buf_len) {
        return -1;
    }

    if (out_chunk->is_parent) {
        size_t consumed = 0;
        size_t cap = 0;
        while (consumed < out_chunk->content_len) {
            if (out_chunk->child_count == cap) {
                size_t new_cap = cap == 0 ? 4 : cap * 2;
                ThreediChunk *tmp = (ThreediChunk *)realloc(out_chunk->children, new_cap * sizeof(ThreediChunk));
                if (!tmp) {
                    return -1;
                }
                out_chunk->children = tmp;
                cap = new_cap;
            }
            size_t child_advance = 0;
            if (parse_chunk(buf, buf_len, content_start + consumed, &out_chunk->children[out_chunk->child_count], &child_advance) != 0) {
                return -1;
            }
            consumed += child_advance;
            out_chunk->child_count++;
        }
        if (consumed != out_chunk->content_len) {
            return -1;
        }
    } else {
        out_chunk->data = buf + content_start;
        out_chunk->data_len = out_chunk->content_len;
    }

    if (advance) {
        *advance = out_chunk->content_len + 8;
    }
    return 0;
}

const char *threedi_version(void)
{
    return "threedi-0.1.0-dev";
}

int threedi_smoke_self_check(void)
{
    return 1;
}

static int parse_owned_buffer(uint8_t *buf, size_t len, ThreediFile *out_file)
{
    if (!buf || !out_file) {
        free(buf);
        return EINVAL;
    }
    if (len < 8 || memcmp(buf, "3DI3", 4) != 0) {
        free(buf);
        return -1;
    }
    uint32_t version = (uint32_t)buf[4]
                     | ((uint32_t)buf[5] << 8)
                     | ((uint32_t)buf[6] << 16)
                     | ((uint32_t)buf[7] << 24);

    ThreediChunk root = {0};
    size_t root_advance = 0;
    if (parse_chunk(buf, len, 8, &root, &root_advance) != 0) {
        free_chunk(&root);
        free(buf);
        return -1;
    }

    out_file->version = version;
    out_file->buffer = buf;
    out_file->buffer_len = len;
    out_file->root = (ThreediChunk *)malloc(sizeof(ThreediChunk));
    if (!out_file->root) {
        free_chunk(&root);
        free(buf);
        out_file->buffer = NULL;
        out_file->buffer_len = 0;
        return -1;
    }
    memcpy(out_file->root, &root, sizeof(ThreediChunk));
    return 0;
}

int threedi_read_file(const char *path, ThreediFile *out_file)
{
    if (!path || !out_file) {
        return EINVAL;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        return errno ? errno : -1;
    }

    int rc = 0;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    long flen = ftell(f);
    if (flen < 0) {
        fclose(f);
        return -1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }

    uint8_t *buf = (uint8_t *)malloc((size_t)flen);
    if (!buf) {
        fclose(f);
        return -1;
    }
    if (fread(buf, 1, (size_t)flen, f) != (size_t)flen) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);

    return parse_owned_buffer(buf, (size_t)flen, out_file);
}

int threedi_read_memory(const uint8_t *data, size_t size, ThreediFile *out_file)
{
    if (!data || size == 0 || !out_file) {
        return EINVAL;
    }

    uint8_t *buf = (uint8_t *)malloc(size);
    if (!buf) {
        return -1;
    }
    memcpy(buf, data, size);
    return parse_owned_buffer(buf, size, out_file);
}

void threedi_free_file(ThreediFile *file)
{
    if (!file || !file->root) {
        return;
    }
    free_chunk(file->root);
    free(file->root);
    file->root = NULL;
    free(file->buffer);
    file->buffer = NULL;
    file->buffer_len = 0;
}

int threedi_write_file(const char *path, const ThreediFile *file)
{
    if (!path || !file || !file->buffer || file->buffer_len == 0) {
        return EINVAL;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        return errno ? errno : -1;
    }
    size_t written = fwrite(file->buffer, 1, file->buffer_len, f);
    fclose(f);
    if (written != file->buffer_len) {
        return -1;
    }
    return 0;
}
