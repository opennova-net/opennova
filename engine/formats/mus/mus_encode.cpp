/* MUS bytecode file writer: the SCR0 file in MDEdit's layout from a compiled model (mus_encode_file), split from the
   compiler (mus_compile.cpp), which makes the model from the text. Its witnesses are in the writer's own comment. */

#include <formats/mus/mus.h>

#include <base/io/le.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace opennova::mus {

/* Append `n` bytes from `src` to `*buf`, growing as needed. */
static void buf_append(uint8_t **buf, size_t *cap, size_t *used,
                       const void *src, size_t n) {
    if (*used + n > *cap) {
        size_t nc = *cap ? *cap : 256;
        while (nc < *used + n) nc *= 2;
        *buf = (uint8_t *)realloc(*buf, nc);
        *cap = nc;
    }
    if (n) memcpy(*buf + *used, src, n);
    *used += n;
}
static void buf_append_zero(uint8_t **buf, size_t *cap, size_t *used, size_t n) {
    if (*used + n > *cap) {
        size_t nc = *cap ? *cap : 256;
        while (nc < *used + n) nc *= 2;
        *buf = (uint8_t *)realloc(*buf, nc);
        *cap = nc;
    }
    if (n) memset(*buf + *used, 0, n);
    *used += n;
}

/* Zeros to the next multiple of 16 from the chunk's start (MDEdit's debug section, sub_40DF30 @ 0x40E23C..0x40E24B,
   and the chunk's end, its file's length v37 @ 0x40E655). */
static void buf_align16_from(uint8_t **buf, size_t *cap, size_t *used, size_t chunk_at) {
    buf_append_zero(buf, cap, used, (16 - (*used - chunk_at) % 16) % 16);
}

/* One 48-byte debug entry: a value at +0 (and +8), a 32-byte name at +0x10. */
static void debug_entry(uint8_t **buf, size_t *cap, size_t *used, uint32_t first, uint32_t third, const char *name) {
    uint8_t entry[48];
    memset(entry, 0, sizeof(entry));
    io::write_u32_le(entry + 0x00, first);
    io::write_u32_le(entry + 0x08, third);
    const size_t nlen = strnlen(name, 31);
    memcpy(entry + 0x10, name, nlen);
    buf_append(buf, cap, used, entry, sizeof(entry));
}

/* The SCR0 file in MDEdit's layout (above; MDEdit.exe's chunk writer sub_40DF30 @ 0x40DF30 and linker sub_40E6E0 @
   0x40E6E0): the 44-byte header and the chunk pointer table; each MU01 chunk's 0x48-byte header and 0x20 bytes of
   zeros, its section table (chunk-relative entry PCs), its code and its string table (a chunk-relative offset a
   string, then each string and its NUL); zeros to the chunk's next 16, then the editor debug section:
   the 256-byte source path, a 0x50-byte header of the five tables' entry size, offsets and counts, the sections'
   (bytecode-relative entries), the globals', the parameters' and the functions' 48-byte entries and the 8-byte
   lines, then zeros to the chunk's next 16 (each chunk MDEdit's own file, which its linker puts down one after the
   other); after the last, the method names' resolve table (zeros the loader fills) and the names, each its length
   byte (itself, the name and its NUL), the name and its NUL. Each field as the model holds it: the chunk's sizes and
   frame base, the table the scripts hold. The runtime reads the header, the section table, the code and the names
   [orig: AudioVM_LoadScriptFile @ 0x672D20, AudioVM_FixupPointers @ 0x672470, ScriptInstance_Init @ 0x672EF0];
   MDEdit's reader the rest. */
int mus_encode_file(const MusScript *const *scripts, uint32_t script_count,
                               uint8_t **out_buf, size_t *out_size) {
    if (!scripts || !out_buf || !out_size) return -1;
    for (uint32_t i = 0; i < script_count; ++i)
        if (!scripts[i]) return -3;

    /* The method table: the file holds one, every chunk's `method` calls resolving through it [orig:
       ScriptInstance_Init @ 0x672FAF], so the scripts written together hold the same (-4 otherwise); each name ends
       within its slot (-5 otherwise: its length byte past 127, read signed, would step the loader back [orig:
       AudioVM_LoadScriptFile @ 0x672DEB..0x672DFC]). */
    const MusScript *table = script_count ? scripts[0] : NULL;
    const uint32_t name_count = table ? table->intrinsic_count : 0;
    if (name_count && !table->intrinsic_names) return -4;
    for (uint32_t i = 1; i < script_count; ++i) {
        const MusScript *s = scripts[i];
        if (s->intrinsic_count != name_count || (name_count && !s->intrinsic_names)) return -4;
        for (uint32_t k = 0; k < name_count; ++k)
            if (strncmp(s->intrinsic_names[k], table->intrinsic_names[k], MUS_METHOD_NAME_SIZE) != 0) return -4;
    }
    for (uint32_t k = 0; k < name_count; ++k)
        if (strnlen(table->intrinsic_names[k], MUS_METHOD_NAME_SIZE) == MUS_METHOD_NAME_SIZE) return -5;

    uint8_t *buf = NULL;
    size_t   cap = 0, used = 0;

    buf_append_zero(&buf, &cap, &used, sizeof(MusFileHeader));
    size_t chunk_table_offset = used;
    buf_append_zero(&buf, &cap, &used, (size_t)script_count * 4);

    uint32_t *chunk_offs = (uint32_t *)calloc(script_count ? script_count : 1, sizeof(uint32_t));
    if (!chunk_offs) { free(buf); return -2; }

    for (uint32_t i = 0; i < script_count; ++i) {
        const MusScript *s = scripts[i];

        chunk_offs[i] = (uint32_t)used;
        const size_t chunk_hdr_at = used;
        buf_append_zero(&buf, &cap, &used, sizeof(MusChunkHeader) + 0x20);

        const size_t sec_tab_at = used;
        buf_append_zero(&buf, &cap, &used, (size_t)s->section_count * 4);
        const size_t bc_at = used;
        const uint32_t bc_rel = (uint32_t)(bc_at - chunk_hdr_at);
        if (s->code && s->code_size > 0) buf_append(&buf, &cap, &used, s->code, s->code_size);
        for (uint32_t k = 0; k < s->section_count; ++k) io::write_u32_le(buf + sec_tab_at + 4 * k, bc_rel + s->sections[k].code_offset);
        /* The strings `pushstr` pushes [orig: AudioVM_Op_PushStr @ 0x672836] where MDEdit writes them: the table at
           the code's end (its padding to four the code's own), a chunk-relative offset a string, then the strings
           packed, each to its NUL (MDEdit.exe sub_40DF30 @ 0x40E183..0x40E231). */
        const size_t strings_table_at = used;
        buf_append_zero(&buf, &cap, &used, (size_t)s->string_count * 4);
        for (uint32_t k = 0; k < s->string_count; ++k) {
            io::write_u32_le(buf + strings_table_at + 4 * k, (uint32_t)(used - chunk_hdr_at));
            const char *text = s->strings && s->strings[k] ? s->strings[k] : "";
            buf_append(&buf, &cap, &used, text, strlen(text) + 1);
        }

        /* The editor debug section, at the chunk's next 16. */
        buf_align16_from(&buf, &cap, &used, chunk_hdr_at);
        const size_t str_at = used;
        buf_append_zero(&buf, &cap, &used, 256);
        size_t plen = strnlen(s->source_path, 255);
        memcpy(buf + str_at, s->source_path, plen);
        const size_t hdr_at = used;
        buf_append_zero(&buf, &cap, &used, 0x50);
        const auto rel = [&](size_t at) { return (uint32_t)(at - chunk_hdr_at); };
        const size_t sections_at = used;
        for (uint32_t k = 0; k < s->section_count; ++k)
            debug_entry(&buf, &cap, &used, s->sections[k].code_offset, 0, s->sections[k].name);
        const size_t vars_at = used;
        for (uint32_t k = 0; k < s->variable_count; ++k)
            debug_entry(&buf, &cap, &used, s->variables[k].byte_offset, 0, s->variables[k].name);
        const size_t locals_at = used;
        for (uint32_t k = 0; k < s->local_count; ++k)
            debug_entry(&buf, &cap, &used, s->locals[k].frame_offset, 0, s->locals[k].name);
        const size_t functions_at = used;
        for (uint32_t k = 0; k < s->function_count; ++k)
            debug_entry(&buf, &cap, &used, s->functions[k].start, s->functions[k].end, s->functions[k].name);
        const size_t lines_at = used;
        for (uint32_t k = 0; k < s->line_count; ++k) {
            uint8_t pair[8];
            io::write_u32_le(pair, s->lines[k].code_offset);
            io::write_u32_le(pair + 4, s->lines[k].line);
            buf_append(&buf, &cap, &used, pair, sizeof(pair));
        }
        buf_align16_from(&buf, &cap, &used, chunk_hdr_at);   /* the chunk's end */
        uint8_t *h = buf + hdr_at;
        io::write_u32_le(h + 0x00, 48);
        io::write_u32_le(h + 0x04, rel(sections_at));
        io::write_u32_le(h + 0x08, s->section_count);
        io::write_u32_le(h + 0x0C, rel(vars_at));
        io::write_u32_le(h + 0x10, s->variable_count);
        io::write_u32_le(h + 0x14, rel(locals_at));
        io::write_u32_le(h + 0x18, s->local_count);
        io::write_u32_le(h + 0x1C, rel(functions_at));
        io::write_u32_le(h + 0x20, s->function_count);
        io::write_u32_le(h + 0x24, rel(lines_at));
        io::write_u32_le(h + 0x28, s->line_count);

        MusChunkHeader ch;
        memset(&ch, 0, sizeof(ch));
        ch.tag                  = MUS_CHUNK_TAG_MU01;
        ch.version              = 0x00000100;
        memcpy(ch.name, s->name, strnlen(s->name, MUS_NAME_SIZE));
        ch.globals_size         = s->globals_size;
        ch.locals_size          = s->locals_size;
        ch.bytecode_offset      = bc_rel;
        ch.section_table_offset = rel(sec_tab_at);
        ch.section_count        = s->section_count;
        ch.entry_section_index  = s->entry_section_index;
        ch.strings_offset       = rel(strings_table_at);
        ch.string_count         = s->string_count;
        ch.debug_offset         = rel(str_at);
        /* +0x3C doubles as the `enter` frame base (instance[+0x3C], added as it is [orig: AudioVM_Op_Enter @
           0x672C39]); 0x20 in every MDEdit file. */
        ch.frame_base           = s->locals_frame_offset;
        /* +0x40 the MessageHandler entry, +0x44 the code's start [orig: AudioVM_FixupPointers @ 0x672495 /
           @ 0x6724a1]. */
        ch.message_handler_offset = s->has_message_handler ? bc_rel + s->message_handler_offset : 0;
        ch.main_entry_offset    = bc_rel;
        memcpy(buf + chunk_hdr_at, &ch, sizeof(ch));
    }

    /* The method names' resolve table (a slot per name the loader fills) and the names, each its length (the
       byte, the name, its NUL) then the name and its NUL, right after the last chunk (MDEdit.exe sub_40E6E0 @
       0x40E6E0: the chunks' lengths summed from 44 and the chunk table). */
    size_t resolve_at = used;
    buf_append_zero(&buf, &cap, &used, (size_t)name_count * 4);
    size_t strings_at = used;
    for (uint32_t i = 0; i < name_count; ++i) {
        const char *name = table->intrinsic_names[i];
        size_t nlen = strlen(name);
        uint8_t L = (uint8_t)(nlen + 2);
        buf_append(&buf, &cap, &used, &L, 1);
        buf_append(&buf, &cap, &used, name, nlen);
        uint8_t z = 0;
        buf_append(&buf, &cap, &used, &z, 1);
    }

    MusFileHeader fh;
    memset(&fh, 0, sizeof(fh));
    fh.magic                     = MUS_MAGIC_SCR0;
    fh.version                   = 0x00000100;
    fh.chunk_count               = script_count;
    fh.chunk_table_offset        = (uint32_t)chunk_table_offset;
    fh.name_count                = name_count;
    fh.name_strings_blob_offset  = (uint32_t)strings_at;
    fh.name_resolve_table_offset = (uint32_t)resolve_at;
    memcpy(buf, &fh, sizeof(fh));
    for (uint32_t i = 0; i < script_count; ++i) io::write_u32_le(buf + chunk_table_offset + i * 4, chunk_offs[i]);
    free(chunk_offs);

    *out_buf = buf;
    *out_size = used;
    return 0;
}

} // namespace opennova::mus
