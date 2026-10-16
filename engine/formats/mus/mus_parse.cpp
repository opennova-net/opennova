/* MUS container parser implementation.

   [orig: AudioVM_LoadScriptFile @ 0x672D20, AudioVM_FixupPointers @ 0x672470; docs/audio/mus-sbf-re.md]
   SCR0 file header parser at Jointops.exe!AudioVM_LoadScriptFile @ 0x00672D20.
   MU01 chunk pointer fixup at Jointops.exe!AudioVM_FixupPointers @ 0x00672470. */

#include <formats/mus/mus.h>

#include <base/io/os_path.h>

#include <base/io/le.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

namespace opennova::mus {

/* Witnessed: Jointops.exe!AudioVM_LoadScriptFile @ 0x00672D20 (magic compare vs 'SCR0' 0x30524353).
   Engine itself only validates the SCR0 magic; we additionally bound the chunk
   count to a sane limit so a corrupt file does not provoke a runaway alloc. The
   name count needs no bound of ours: its resolve table must lie in the file
   (mus_open_memory). */
int mus_validate(const uint8_t *data, size_t size) {
    if (!data || size < sizeof(MusFileHeader)) return -1;
    MusFileHeader h;
    memcpy(&h, data, sizeof(h));
    if (h.magic != MUS_MAGIC_SCR0) return -2;
    if (h.chunk_count > 1024) return -3;
    return 0;
}

/* Pick the smallest chunk-relative offset strictly greater than `lo` from a
   list of candidates, falling back to `fallback` if none. Used to bound the
   bytecode region: the engine relocates these offsets but never stores an
   explicit bytecode-size; the next-thing-after-bytecode is whichever chunk-
   relative offset comes first beyond bytecode_offset. */
static uint32_t smallest_after(uint32_t lo, const uint32_t *cands, size_t n,
                               uint32_t fallback) {
    uint32_t best = fallback;
    int found = 0;
    for (size_t i = 0; i < n; ++i) {
        if (cands[i] > lo && cands[i] <= fallback) {
            if (!found || cands[i] < best) { best = cands[i]; found = 1; }
        }
    }
    return best;
}

/* MDEdit-authored MUS files carry an editor debug section at MU01 + `debug_offset`: the 256-byte
   source path, then a 0x50-byte header of five tables, each a chunk-relative offset and a count after the
   entry size (+0 entry size, 48; +4/+8 the sections, +0xC/+0x10 the globals, +0x14/+0x18 the functions'
   parameters, +0x1C/+0x20 the functions, +0x24/+0x28 the lines; the rest zero), then the tables themselves.
   A section's, a global's and a parameter's entry: its offset at +0 (a section's bytecode-relative, a
   global's its byte in the globals area, a parameter's its frame offset) and its 32-byte name at +0x10; a
   function's: its start at +0 and its end at +8 (bytecode-relative), its name at +0x10; a line's: 8 bytes,
   the instruction's bytecode-relative offset and the source line. The runtime relocates this region but
   never reads it; only MDEdit and the decompiler care. A file of an older header (no table offsets) has its
   section and global entries one after the other past the header. */
static void copy_name(char *dst, size_t cap, const uint8_t *src, size_t max) {
    if (max > cap - 1) max = cap - 1;
    size_t k = 0;
    while (k < max && src[k] != 0) ++k;
    memcpy(dst, src, k);
    dst[k] = 0;
}

static void parse_debug_export(const uint8_t *data, size_t size,
                               uint32_t chunk_off,
                               const MusChunkHeader *ch,
                               MusScript *out) {
    if (ch->debug_offset == 0) return;
    uint64_t base64 = (uint64_t)chunk_off + ch->debug_offset;
    if (base64 + 256 > size) return;
    uint32_t base = (uint32_t)base64;
    /* The path slot: tolerate leading NULs, copy up to the first NUL after them. */
    uint32_t start = base;
    while (start < base + 256 && data[start] == 0) ++start;
    uint32_t copy_end = start;
    while (copy_end < base + 256 && data[copy_end] != 0) ++copy_end;
    uint32_t copy_len = copy_end - start;
    if (copy_len >= MUS_SOURCE_PATH_SIZE) copy_len = MUS_SOURCE_PATH_SIZE - 1;
    memcpy(out->source_path, data + start, copy_len);
    out->source_path[copy_len] = 0;

    uint32_t hdr = base + 256;
    if ((uint64_t)hdr + 0x50 > size) return;
    uint32_t entry_size = io::read_u32_le(data + hdr);
    if (entry_size == 0) entry_size = 48;
    if (entry_size < 0x10 || entry_size > 256) return;
    const uint32_t section_count = io::read_u32_le(data + hdr + 0x08), var_count = io::read_u32_le(data + hdr + 0x10);
    const uint32_t local_count = io::read_u32_le(data + hdr + 0x18), function_count = io::read_u32_le(data + hdr + 0x20);
    const uint32_t line_count = io::read_u32_le(data + hdr + 0x28);
    if (section_count > 1024 || var_count > 1024 || local_count > 1024 || function_count > 1024 ||
        line_count > (1u << 20))
        return;
    /* Each table where its offset says, else (an older header) right after the one before. */
    uint64_t next = (uint64_t)hdr + 0x50;
    const auto table_at = [&](uint32_t field, uint64_t count, uint64_t stride) -> uint64_t {
        const uint32_t rel = io::read_u32_le(data + hdr + field);
        const uint64_t at = rel ? (uint64_t)chunk_off + rel : next;
        next = at + count * stride;
        return next <= size ? at : 0;
    };
    const uint64_t sections_at = table_at(0x04, section_count, entry_size);
    const uint64_t vars_at = table_at(0x0C, var_count, entry_size);
    const uint64_t locals_at = local_count ? table_at(0x14, local_count, entry_size) : 0;
    const uint64_t functions_at = function_count ? table_at(0x1C, function_count, entry_size) : 0;
    const uint64_t lines_at = line_count ? table_at(0x24, line_count, 8) : 0;
    const size_t name_max = entry_size - 0x10;

    /* Section-name entries, by the section table's order: the editor's labels (an empty one stays empty). */
    if (sections_at) {
        const uint32_t named = section_count < out->section_count ? section_count : out->section_count;
        for (uint32_t i = 0; i < named; ++i) {
            const uint8_t *e = data + sections_at + (uint64_t)i * entry_size;
            if (e[0x10] != 0) copy_name(out->sections[i].name, MUS_SECTION_NAME_SIZE, e + 0x10, name_max);
        }
    }
    /* Globals, one per declared global (Var00..Var15 and the user's). */
    if (vars_at && var_count > 0) {
        out->variables = (MusVariable *)calloc(var_count, sizeof(MusVariable));
        if (!out->variables) return;
        out->variable_count = var_count;
        for (uint32_t i = 0; i < var_count; ++i) {
            const uint8_t *e = data + vars_at + (uint64_t)i * entry_size;
            out->variables[i].byte_offset = io::read_u32_le(e);
            copy_name(out->variables[i].name, MUS_GLOBAL_NAME_SIZE, e + 0x10, name_max);
        }
    }
    if (locals_at) {
        out->locals = (MusLocal *)calloc(local_count, sizeof(MusLocal));
        if (!out->locals) return;
        out->local_count = local_count;
        for (uint32_t i = 0; i < local_count; ++i) {
            const uint8_t *e = data + locals_at + (uint64_t)i * entry_size;
            out->locals[i].frame_offset = io::read_u32_le(e);
            copy_name(out->locals[i].name, MUS_SECTION_NAME_SIZE, e + 0x10, name_max);
        }
    }
    if (functions_at) {
        out->functions = (MusFunction *)calloc(function_count, sizeof(MusFunction));
        if (!out->functions) return;
        out->function_count = function_count;
        for (uint32_t i = 0; i < function_count; ++i) {
            const uint8_t *e = data + functions_at + (uint64_t)i * entry_size;
            out->functions[i].start = io::read_u32_le(e);
            out->functions[i].end = io::read_u32_le(e + 8);
            copy_name(out->functions[i].name, MUS_SECTION_NAME_SIZE, e + 0x10, name_max);
        }
    }
    if (lines_at) {
        out->lines = (MusLine *)calloc(line_count, sizeof(MusLine));
        if (!out->lines) return;
        out->line_count = line_count;
        for (uint32_t i = 0; i < line_count; ++i) {
            out->lines[i].code_offset = io::read_u32_le(data + lines_at + 8ull * i);
            out->lines[i].line = io::read_u32_le(data + lines_at + 8ull * i + 4);
        }
    }
}

/* Witnessed: Jointops.exe!AudioVM_FixupPointers @ 0x00672470, driven by the chunk-pointer
   relocation loop in AudioVM_LoadScriptFile @ 0x00672D20. Reads one MU01 chunk into the
   parsed MusScript form. `chunk_off` is the file offset of the MU01 header. */
static int parse_chunk(const uint8_t *data, size_t size, uint32_t chunk_off,
                       MusScript *out) {
    if ((uint64_t)chunk_off + sizeof(MusChunkHeader) > size) return -10;
    MusChunkHeader ch;
    memcpy(&ch, data + chunk_off, sizeof(ch));

    memset(out, 0, sizeof(*out));
    memcpy(out->name, ch.name, MUS_NAME_SIZE);
    out->globals_size        = ch.globals_size;
    out->locals_size         = ch.locals_size;
    out->entry_section_index = ch.entry_section_index;
    /* `enter` frame base = instance[+0x3C] (witnessed @ AudioVM_Op_Enter
       0x672C20), the chunk's frame_base field. MDEdit emits 0x20; carry it
       through so the VM matches the binary for any chunk. */
    out->locals_frame_offset = ch.frame_base;
    /* The strings `pushstr` reads [orig: AudioVM_Op_PushStr @ 0x672836]: their
       count alone (the writer refuses a script holding any). */
    out->string_count = ch.string_count;

    /* Section table: section_count uint32 chunk-relative entry-PCs. We
       normalise these to bytecode-relative offsets (subtract bytecode_offset)
       so callers can index directly into MusScript.code. The original on-disk
       layout stores chunk-relative offsets (e.g. section[0]=0x89 when the
       bytecode region begins at chunk+0x88). */
    if (ch.section_count > 0) {
        if ((uint64_t)chunk_off + ch.section_table_offset
            + (uint64_t)ch.section_count * 4 > size) return -11;
        out->sections = (MusSection *)calloc(ch.section_count, sizeof(MusSection));
        if (!out->sections) return -5;
        out->section_count = ch.section_count;
        const uint8_t *sec_base = data + chunk_off + ch.section_table_offset;
        for (uint32_t i = 0; i < ch.section_count; ++i) {
            uint32_t code_off;
            memcpy(&code_off, sec_base + i * 4, 4);
            /* Convert chunk-relative -> bytecode-relative. */
            if (code_off >= ch.bytecode_offset) code_off -= ch.bytecode_offset;
            out->sections[i].code_offset = code_off;
            /* The name is the debug table's (parse_debug_export), empty where the
               file has none: the game reads no label (mus_section_label names one
               for display). */
        }
    }

    /* Bytecode: chunk-relative file region whose size is the gap between
       bytecode_offset and the next-thing chunk-relative offset (debug info,
       string section, or section table if it sits after bytecode). The
       engine never stores an explicit bytecode_size; this matches the
       layout pattern seen in jo_gamemus.bin (bytecode 0x88..0x10C = 132 B). */
    if (ch.bytecode_offset > 0) {
        if ((uint64_t)chunk_off + ch.bytecode_offset > size) return -12;
        uint32_t chunk_max = (uint32_t)(size - chunk_off);
        uint32_t cands[] = {
            ch.section_table_offset,
            ch.strings_offset,
            ch.debug_offset,
        };
        uint32_t end = smallest_after(ch.bytecode_offset, cands,
                                      sizeof(cands) / sizeof(cands[0]),
                                      chunk_max);
        if (end <= ch.bytecode_offset) return -13;
        uint32_t code_size = end - ch.bytecode_offset;
        out->code = (uint8_t *)malloc(code_size);
        if (!out->code) return -5;
        memcpy(out->code, data + chunk_off + ch.bytecode_offset, code_size);
        out->code_size = code_size;
    }

    /* The MessageHandler entry: the chunk's +0x40 code pointer, relocated when
       nonzero [orig: AudioVM_FixupPointers @ 0x672495 vmData[16]] and jumped
       to by the restart frame [orig: sub_672E50 @ 0x672eba]. Normalised to a
       bytecode-relative PC like the section entries (retail gamemus 0x91 with
       bytecode at 0x88 -> 0x09; menumus 0x94 = its bytecode start -> 0). A
       pointer outside the bytecode region is treated as no handler. */
    if (ch.message_handler_offset != 0
        && ch.message_handler_offset >= ch.bytecode_offset
        && ch.message_handler_offset - ch.bytecode_offset < out->code_size) {
        out->message_handler_offset = ch.message_handler_offset - ch.bytecode_offset;
        out->has_message_handler = 1;
    }

    /* Editor debug section: source path + section name + variable name table.
       Optional; absent in stripped runtime files. */
    parse_debug_export(data, size, chunk_off, &ch, out);

    return 0;
}

/* Witnessed: Jointops.exe!AudioVM_LoadScriptFile @ 0x00672D20.
   Memory-mode parser. Walks SCR0 header, reads chunk pointer table, parses
   each MU01 chunk into a MusScript, and copies the file-level intrinsic name
   table out of its Pascal-style strings blob. */
int mus_open_memory(MusFile *out, const uint8_t *data, size_t size) {
    if (!out || !data) return -1;
    int v = mus_validate(data, size);
    if (v != 0) return v;

    memset(out, 0, sizeof(*out));
    memcpy(&out->header, data, sizeof(out->header));

    /* Chunk pointer table: chunk_count uint32 file offsets. */
    uint32_t n = out->header.chunk_count;
    if (n > 0) {
        if ((uint64_t)out->header.chunk_table_offset + (uint64_t)n * 4 > size) {
            mus_close(out);
            return -20;
        }
        out->scripts = (MusScript *)calloc(n, sizeof(MusScript));
        if (!out->scripts) { mus_close(out); return -5; }
        const uint8_t *table = data + out->header.chunk_table_offset;
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t chunk_off;
            memcpy(&chunk_off, table + i * 4, 4);
            int rc = parse_chunk(data, size, chunk_off, &out->scripts[i]);
            if (rc != 0) { mus_close(out); return rc; }
        }
    }

    /* The method-name table, read as the loader reads it [orig: AudioVM_LoadScriptFile @ 0x672DBD..0x672E01]:
       when the count is not 0, from the blob's start, each name the bytes past its length byte up to its NUL
       (the resolver strcmp's it [orig: AudioVM_FindContextByName @ 0x6723F0]), the next one its length byte on,
       the byte read signed [orig: @ 0x672DEB movsx; @ 0x672DFC add esi, ecx]; a count the loop takes as
       negative reads none (jle @ 0x672DDB). MDEdit writes each length byte as itself, the name and its NUL
       (`07 'G' 'E' 'c' 'h' 'o' 00`). The loader writes each name's handler into the resolve table in the
       file's own buffer [orig: @ 0x672DE9 mov [edi], eax], so a table reaching past the file's end is refused
       (-21: the game writes past its buffer), as is a name read from outside the file (-22) or with no NUL
       before its end (-23); a name longer than a MUS_METHOD_NAME_SIZE slot holds (only where its length byte
       does not span it) is past what the writer can write back (-24). */
    const int32_t name_count = (int32_t)out->header.name_count;
    if (name_count > 0) {
        if ((uint64_t)out->header.name_resolve_table_offset + 4ull * (uint64_t)name_count > size) {
            mus_close(out);
            return -21;
        }
        out->intrinsic_names = (char (*)[MUS_METHOD_NAME_SIZE])calloc((size_t)name_count, MUS_METHOD_NAME_SIZE);
        if (!out->intrinsic_names) { mus_close(out); return -5; }
        out->intrinsic_count = (uint32_t)name_count;
        int64_t cursor = out->header.name_strings_blob_offset;
        for (int32_t i = 0; i < name_count; ++i) {
            if (cursor < 0 || (uint64_t)cursor + 1 >= size) { mus_close(out); return -22; }
            const char *name = (const char *)data + cursor + 1;
            const size_t room = size - (size_t)cursor - 1;
            const size_t len = strnlen(name, room);
            if (len == room) { mus_close(out); return -23; }
            if (len >= (size_t)MUS_METHOD_NAME_SIZE) { mus_close(out); return -24; }
            memcpy(out->intrinsic_names[i], name, len);
            cursor += (int8_t)data[cursor];
        }
    }

    /* Each script a copy of the file's table, so the decompiler and the VM resolve a method number from a single
       MusScript. */
    for (uint32_t s = 0; out->scripts && s < out->header.chunk_count && out->intrinsic_count > 0; ++s) {
        MusScript &script = out->scripts[s];
        const size_t bytes = (size_t)out->intrinsic_count * MUS_METHOD_NAME_SIZE;
        script.intrinsic_names = (char (*)[MUS_METHOD_NAME_SIZE])malloc(bytes);
        if (!script.intrinsic_names) { mus_close(out); return -5; }
        memcpy(script.intrinsic_names, out->intrinsic_names, bytes);
        script.intrinsic_count = out->intrinsic_count;
    }

    return 0;
}

/* Witnessed: Jointops.exe!AudioVM_LoadScriptFile @ 0x00672D20 (CreateFile + ReadFile
   slurp at the head of the function). The engine reads the whole file into RAM
   before relocation; we mirror that by slurp + delegate. */
int mus_open(MusFile *out, const char *path) {
    if (!out || !path) {
        if (out) memset(out, 0, sizeof(*out));
        return -1;
    }
    memset(out, 0, sizeof(*out));
    FILE *f = opennova::io::fopen_utf8(path, "rb");
    if (!f) return -30;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return -31; }
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (!buf) { fclose(f); return -5; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf); fclose(f); return -32;
    }
    fclose(f);
    int rc = mus_open_memory(out, buf, (size_t)n);
    free(buf);    /* mus_open_memory copied data we needed into owned buffers */
    return rc;
}

std::string mus_section_label(const MusScript *s, uint32_t index) {
    if (!s || index >= s->section_count) return std::string();
    const char *name = s->sections[index].name;
    if (name[0]) return std::string(name, strnlen(name, MUS_SECTION_NAME_SIZE));
    return "Section_" + std::to_string(index);
}

const MusSection *mus_find_section(const MusScript *s, const char *name) {
    if (!s || !name) return NULL;
    for (uint32_t i = 0; i < s->section_count; ++i) {
        if (mus_section_label(s, i) == name) {
            return &s->sections[i];
        }
    }
    return NULL;
}

void mus_close(MusFile *file) {
    if (!file) return;
    free(file->intrinsic_names);
    if (file->scripts) {
        for (uint32_t i = 0; i < file->header.chunk_count; ++i) {
            mus_script_free(&file->scripts[i]);
        }
        free(file->scripts);
        file->scripts = NULL;
    }
    memset(file, 0, sizeof(*file));
}

} // namespace opennova::mus
