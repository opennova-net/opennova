#ifndef MUS_DECODE_H
#define MUS_DECODE_H

/* Internal shared MUS bytecode decoder.

   Single source of truth for the IDA-derived opcode table + operand widths, so
   the source decompiler (mus_decompile.cpp) and the editor-facing structural
   model (mus_model.cpp) decode bytecode identically and can never drift. Was
   previously private to mus_decompile.cpp; lifted here verbatim (behaviour
   preserved, guarded by the golden byte-compare in mus_decompile_test.cpp).

   Header-only with internal (`static`) linkage: each translation unit that
   includes this gets its own copy of the table/functions, so there is no ODR or
   multiple-definition conflict. Names are kept at the original (unqualified)
   spelling so mus_decompile.cpp needed only an #include + deletion of the moved
   definitions, with no reference renames. */

#include "mus/mus.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Operand types match the Python decompiler:
     0 = u8 (8-bit immediate)
     1 = u16 (16-bit immediate)
     2 = u32 (32-bit immediate)
     3 = u8 global var byte-offset
     4 = u8 local var index
     5 = u32 label / branch target offset
     6 = u8 function index (callvl)
     7 = u8 special (tablexec inner_opcode)
     9 = u8 method index */
struct OpInfo {
    const char *mnemonic;
    int         operand_count;
    int         operand_types[4];
};

#define MAX_OPERANDS 5    /* tablexec: 4 immediate + 1 dynamic table-data slot */

static const OpInfo kOps[256] = {
    /* 0x00 */ {"nop",      0, {0}},
    /* 0x01 */ {"push",     1, {0}},
    /* 0x02 */ {"push",     1, {2}},
    /* 0x03 */ {"push_g",   1, {3}},
    /* 0x04 */ {"push_l",   1, {4}},
    /* 0x05 push_ga: [orig 0x6727F0] movzx byte; inc esi; inc esi -> the engine
       advances IP by 2 (1-byte global offset + 1 reserved byte), NOT u8+u32.
       0x06/0x0A/0x0B likewise advance 2; 0x07 pushstr [0x672830] advances 1. */
    /* 0x05 */ {"push_ga",  2, {3, 0}},
    /* 0x06 */ {"push_la",  2, {4, 0}},
    /* 0x07 */ {"pushstr",  1, {0}},
    /* 0x08 */ {"pop_g",    1, {3}},
    /* 0x09 */ {"pop_l",    1, {4}},
    /* 0x0A */ {"pop_ga",   2, {3, 0}},
    /* 0x0B */ {"pop_la",   2, {4, 0}},
    /* 0x0C */ {"push_me",  0, {0}},
    /* 0x0D */ {NULL,       0, {0}},
    /* 0x0E */ {NULL,       0, {0}},
    /* 0x0F */ {"empty",    0, {0}},
    /* 0x10 */ {"add",      0, {0}},
    /* 0x11 */ {"sub",      0, {0}},
    /* 0x12 */ {"mult",     0, {0}},
    /* 0x13 */ {"div",      0, {0}},
    /* 0x14 */ {"mod",      0, {0}},
    /* 0x15 */ {"l_and",    0, {0}},
    /* 0x16 */ {"l_or",     0, {0}},
    /* 0x17 */ {"and",      0, {0}},
    /* 0x18 */ {"or",       0, {0}},
    /* 0x19 */ {"xor",      0, {0}},
    /* 0x1A */ {"neg",      0, {0}},
    /* 0x1B */ {"inverse",  0, {0}},
    /* 0x1C */ {"lshift",   0, {0}},
    /* 0x1D */ {"rshift",   0, {0}},
    /* 0x1E */ {"not",      0, {0}},
    /* 0x1F */ {NULL,       0, {0}},
    /* 0x20 */ {"equal",    0, {0}},
    /* 0x21 */ {"notequal", 0, {0}},
    /* 0x22 */ {"ge",       0, {0}},
    /* 0x23 */ {"le",       0, {0}},
    /* 0x24 */ {"gt",       0, {0}},
    /* 0x25 */ {"lt",       0, {0}},
    /* 0x26 */ {NULL,       0, {0}},
    /* 0x27 */ {NULL,       0, {0}},
    /* 0x28 */ {"inc_g",    1, {3}},
    /* 0x29 */ {"dec_g",    1, {3}},
    /* 0x2A */ {"inc_l",    1, {4}},
    /* 0x2B */ {"dec_l",    1, {4}},
    /* 0x2C */ {NULL,       0, {0}},
    /* 0x2D */ {NULL,       0, {0}},
    /* 0x2E */ {NULL,       0, {0}},
    /* 0x2F */ {NULL,       0, {0}},
    /* 0x30 */ {"goto",     1, {5}},
    /* 0x31 */ {"brfalse",  1, {5}},
    /* 0x32 */ {"brtrue",   1, {5}},
    /* 0x33 */ {"callvl",   1, {6}},
    /* 0x34 */ {"callv",    1, {5}},
    /* 0x35 */ {"tablexec", 4, {0, 7, 0, 0}},
    /* 0x36 */ {NULL,       0, {0}},
    /* 0x37 */ {NULL,       0, {0}},
    /* 0x38 */ {"enter",    1, {0}},
    /* 0x39 */ {"return",   0, {0}},
    /* 0x3A */ {"yield",    0, {0}},
    /* 0x3B */ {"setstate", 1, {0}},
    /* 0x3C */ {NULL,       0, {0}},
    /* 0x3D */ {"playw",    1, {1}},
    /* 0x3E */ {"play",     1, {0}},
    /* 0x3F */ {"done",     0, {0}},
    /* 0x40 */ {"method",   1, {9}},
    /* 0x41..0xFF default-zero (NULL mnemonic). */
};

struct Instruction {
    uint32_t    offset;
    uint8_t     opcode;
    const char *mnemonic;
    int32_t     operands[MAX_OPERANDS];
    int         operand_count;
    /* Embedded tablexec entry data (count * entry_size bytes). */
    uint8_t    *table_data;
    int         table_count;
    int         table_entry_size;
    uint32_t    size;       /* total bytes including operands and tablexec data */
};

/* Decode the bytecode into Instructions matching the Python disassembler
   semantics (same operand widths and tablexec embedded-data handling). */
static void disassemble(const uint8_t *bytes, uint32_t size,
                        Instruction *out, int max_inst, int *out_count) {
    int n = 0;
    uint32_t pos = 0;
    while (pos < size && n < max_inst) {
        Instruction &inst = out[n];
        memset(&inst, 0, sizeof(inst));
        inst.offset = pos;
        inst.opcode = bytes[pos++];

        const OpInfo &info = kOps[inst.opcode];
        if (info.mnemonic) {
            inst.mnemonic = info.mnemonic;
            for (int k = 0; k < info.operand_count && k < MAX_OPERANDS; ++k) {
                int t = info.operand_types[k];
                int32_t val = 0;
                if (t == 0 || t == 3 || t == 4 || t == 6 || t == 7 || t == 9) {
                    if (pos < size) val = bytes[pos];
                    pos += 1;
                } else if (t == 1) {
                    if (pos + 2 <= size)
                        val = (int32_t)(bytes[pos] | (bytes[pos + 1] << 8));
                    pos += 2;
                } else if (t == 2 || t == 5) {
                    if (pos + 4 <= size) {
                        val = (int32_t)((uint32_t)bytes[pos]
                                       | ((uint32_t)bytes[pos + 1] << 8)
                                       | ((uint32_t)bytes[pos + 2] << 16)
                                       | ((uint32_t)bytes[pos + 3] << 24));
                    }
                    pos += 4;
                } else {
                    if (pos < size) val = bytes[pos];
                    pos += 1;
                }
                inst.operands[inst.operand_count++] = val;
            }
            /* tablexec (0x35): 4 immediates then count*entry_size embedded. */
            if (inst.opcode == 0x35 && inst.operand_count >= 4) {
                int count = inst.operands[0] & 0xFF;
                int es    = inst.operands[2] & 0xFF;
                inst.table_count = count;
                inst.table_entry_size = es;
                int total = count * es;
                if (total > 0 && pos + (uint32_t)total <= size) {
                    inst.table_data = (uint8_t *)malloc((size_t)total);
                    memcpy(inst.table_data, bytes + pos, (size_t)total);
                    pos += (uint32_t)total;
                }
            }
        } else {
            /* Unknown opcode (Python uses "unknown_HH" mnemonic). */
            inst.mnemonic = NULL;
        }
        inst.size = pos - inst.offset;
        ++n;
    }
    *out_count = n;
}

static void free_instructions(Instruction *insts, int count) {
    for (int i = 0; i < count; ++i) {
        free(insts[i].table_data);
    }
}

#endif /* MUS_DECODE_H */
