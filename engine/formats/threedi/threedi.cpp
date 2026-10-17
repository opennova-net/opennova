#include <formats/threedi/threedi.h>

#include <base/io/os_path.h>

#include <formats/threedi/threedi_3di3.h> // THREEDI_3DI3_PARENT_FLAG / _LENGTH_MASK

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <utility>
#include <vector>

namespace opennova::threedi {

// Frees `chunk`'s subtree (not the chunk itself) with an explicit stack.
static void free_chunk(ThreediChunk *chunk)
{
    if (!chunk) {
        return;
    }
    std::vector<std::pair<ThreediChunk *, size_t>> pending;
    pending.push_back({chunk->children, chunk->child_count});
    while (!pending.empty()) {
        const std::pair<ThreediChunk *, size_t> list = pending.back();
        pending.pop_back();
        for (size_t i = 0; i < list.second; ++i) {
            pending.push_back({list.first[i].children, list.first[i].child_count});
        }
        free(list.first);
    }
    chunk->children = NULL;
    chunk->child_count = 0;
}

// The game keeps a model's chunks as nodes in one list it caps at 2000: the list is made for 2000
// nodes [orig: CChunkFile_Construct @ 0x56F520, `push 7D0h` @ 0x56F523], each load empties it
// [orig: CChunkFile_LoadFromFile @ 0x56F780, the reset sub_56F280 @ 0x56F7A5], a ROOT node takes
// index 0 and every chunk read below the file's root appends one, and a node whose index reaches
// 2000 fails [orig: ChunkFile_CreateChildNode @ 0x56F560, ROOT @ 0x56F576..0x56F5C2, the cap
// `cmp ecx, 7D0h; jge` @ 0x56F60E..0x56F614], which ChunkFile_ParseRecursive returns as -3
// (@ 0x56F6D5) and the model loader as a failed load [orig: ThreediGp_LoadFromFile @ 0x5B5780,
// the -1 @ 0x5B5947..0x5B595B]. So at most 1999 chunks below the root read, at any depth.
// (The game reads the tree recursively, a 36-byte frame a chunk [orig: ChunkFile_ParseRecursive
// @ 0x56F6A0, @ 0x56F6F3 / @ 0x56F713], so the cap, not its 1 MiB stack, is what binds.)
static const size_t kMaxChunkNodes = 2000;

// Reads the chunk header at `cursor` into `out` (its id, parent flag, length, offset; a leaf's
// data): false when the header or the content runs past the buffer.
static bool read_chunk_header(const uint8_t *buf, size_t buf_len, size_t cursor, ThreediChunk *out)
{
    memset(out, 0, sizeof(*out));
    if (cursor > buf_len || buf_len - cursor < 8) {
        return false;
    }
    memcpy(out->id, buf + cursor, 4);
    out->id[4] = '\0';
    const uint32_t len_flags = (uint32_t)buf[cursor + 4]
                             | ((uint32_t)buf[cursor + 5] << 8)
                             | ((uint32_t)buf[cursor + 6] << 16)
                             | ((uint32_t)buf[cursor + 7] << 24);
    out->is_parent = (len_flags & THREEDI_3DI3_PARENT_FLAG) != 0;
    out->content_len = len_flags & THREEDI_3DI3_LENGTH_MASK;
    out->offset = cursor;
    if (out->content_len > buf_len - cursor - 8) {
        return false;
    }
    if (!out->is_parent) {
        out->data = buf + cursor + 8;
        out->data_len = out->content_len;
    }
    return true;
}

// The chunk at `cursor` and its subtree, read with an explicit stack (one entry a container being
// read); the 2000th chunk below the root refuses the file, as the game's node cap does
// (kMaxChunkNodes). A container's children must fill its length exactly. On a refusal the tree
// keeps what it read, which the caller frees (free_chunk).
static int parse_chunk(const uint8_t *buf, size_t buf_len, size_t cursor, ThreediChunk *root)
{
    if (!read_chunk_header(buf, buf_len, cursor, root)) {
        return -1;
    }
    struct Container {
        ThreediChunk *chunk;   // stays valid: only the top entry's children array grows
        size_t consumed;       // bytes of its content read so far
        size_t cap;            // its children array's capacity
    };
    std::vector<Container> open;
    if (root->is_parent) {
        open.push_back({root, 0, 0});
    }
    size_t nodes = 1; // the game's ROOT node, index 0
    while (!open.empty()) {
        Container &top = open.back();
        ThreediChunk *parent = top.chunk;
        if (top.consumed >= parent->content_len) {
            if (top.consumed != parent->content_len) {
                return -1;
            }
            open.pop_back();
            continue;
        }
        if (nodes++ >= kMaxChunkNodes) {
            return -1;
        }
        if (parent->child_count == top.cap) {
            const size_t new_cap = top.cap == 0 ? 4 : top.cap * 2;
            ThreediChunk *tmp = (ThreediChunk *)realloc(parent->children, new_cap * sizeof(ThreediChunk));
            if (!tmp) {
                return -1;
            }
            parent->children = tmp;
            top.cap = new_cap;
        }
        ThreediChunk *child = &parent->children[parent->child_count];
        if (!read_chunk_header(buf, buf_len, parent->offset + 8 + top.consumed, child)) {
            return -1;
        }
        parent->child_count++;
        top.consumed += child->content_len + 8;
        if (child->is_parent) {
            open.push_back({child, 0, 0}); // `top` is not used past here
        }
    }
    return 0;
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
    if (parse_chunk(buf, len, 8, &root) != 0) {
        free_chunk(&root);
        free(buf);
        return -1;
    }

    out_file->version = version;
    out_file->buffer = buf;
    out_file->root = (ThreediChunk *)malloc(sizeof(ThreediChunk));
    if (!out_file->root) {
        free_chunk(&root);
        free(buf);
        out_file->buffer = NULL;
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

    FILE *f = opennova::io::fopen_utf8(path, "rb");
    if (!f) {
        return errno ? errno : -1;
    }

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
}

} // namespace opennova::threedi
