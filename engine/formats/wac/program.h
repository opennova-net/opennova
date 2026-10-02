// Compiled WAC program: the faithful bytecode stream plus the rebased operand /
// string pools and the diagnostics gathered during compilation.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <formats/wac/param_type.h>

namespace opennova::mus { struct MusGlobals; }

namespace opennova::wac {

// One compile error in retail's wording. Retail keeps only the first one
// ("%s (%d) %s" into byte_C6EB30, Script_SetCompileError @0x4EE7C0), clears
// it before the load's compiles (WacScript_InitAndLoad @0x4F926A) and a
// console compile (WacScript_ExecuteConsoleCommand @0x4F6D3F), shows it only
// on the script debug overlay (Debug_DrawScriptState @0x4F652A) and never
// refuses the program. `error` marks the lookups that miss in OUR catalogs
// (FX, SOUNDSET, AMMO); Program::ok() answers whether any did, which the
// embedder's WAC wrapper reports, while the program installs and runs as
// retail's does.
struct Diagnostic {
    int line = 0;
    int col = 0;
    std::string message;
    bool error = false;
    uint32_t source = 0; // Program::source_names index
    // Tooling metadata the VM and the listing never read: the byte offset in its source of the
    // token the compiler was at when it reported (the editor places a finding by it; retail
    // keeps the line alone, which counts CRs), and whether the report is a name a table the
    // embedder hands the compiler does not hold (an effect, a sound set or an ammo of the
    // catalogs, a group of the world's table): what it says depends on the files that fill the
    // table, not on the script alone.
    size_t offset = 0;
    bool table = false;
};

// A catalog lookup a pool operand made (an effect, a sound set, an ammo, a text token), found
// or not, at the name's place in its source: tooling metadata the VM and the listing never read
// (the editor's references read it). `name` is the token past its prefix as the tokenizer holds
// it (upper-cased); `offset` and `length` are the same bytes in the source, as written.
// `declaration`: made while a declared name (a VAR's, a CHEAT's, an IF's) was checked as new,
// which resolves the name through the same legs [orig: Script_Compile @ 0x4F3812..0x4F3964,
// @ 0x4F36C0..0x4F380D]: a name the script gives, never one it looks up.
struct CatalogLookup {
    ParamType kind = ParamType::Null; // Fx, SoundSet, Ammo or TextToken
    std::string name;
    uint32_t source = 0; // Program::source_names index
    size_t offset = 0, length = 0;
    bool found = false;
    bool declaration = false;
};

// A word the compiler read as one of its language's, where it stood (tooling metadata the VM and
// the listing never read, as CatalogLookup is; the editor's script device colours them, ADR 0046
// S13 V10): a keyword of the block, declaration and expression syntax (IF, THEN, ELSE, ELSEIF, END
// and its family, ENTER, LEAVE, DOSEQ, DORND, NEXT, GLOOP, PLOOP, VAR, CHEAT, RUN, NOT, AND, OR),
// recognized as the compiler recognizes it (the hash of the token's first four bytes); the name of
// a command of the table (an action or a condition) it emitted; or an operand it looked a name up
// in a catalog for (an effect, a sound set, an ammo, a text key: its whole token, the prefix with
// the name; never a declared name checked as new). `offset` and `length` are the token's bytes in
// its source, as written.
struct WordUse {
    enum class Kind : uint8_t { Keyword, Command, Operand };
    Kind kind = Kind::Keyword;
    uint32_t source = 0; // Program::source_names index
    size_t offset = 0, length = 0;
};

struct InstructionSource {
    uint32_t word = 0;
    uint32_t source_index = 0;
    int line = 0;
};

// A TextToken operand: the pool value indexes these, the key is the authored
// token. [orig: WacScript_ResolveParameter @0x4F2F96 stores the text pointer]
struct TextToken {
    std::string key;
    std::string text;
};

// The catalog lookup behind a pool operand ("FX:NAME", "SS:NAME",
// "AMMO:NAME", "TT:KEY"): tooling metadata only, the VM never reads it.
struct OperandSymbol {
    uint32_t word = 0;
    std::string symbol;
};

struct Program {
    // M# resolves to this context at compilation; absent context uses Scratch.
    std::shared_ptr<opennova::mus::MusGlobals> music_globals;
    std::vector<uint32_t> code;        // instructions + inline operand refs; ends in 0x7A7A7A7A
    // The value pool: slot 0 holds 0, equal values share a slot, 512 slots.
    // [orig: dword_C6AA30, count dword_C69A1C]
    std::vector<int32_t> operands = {0};
    // Text/Filename strings, NUL-terminated and never shared; Text operands
    // carry the byte offset. [orig: byte_C69A20, length dword_C69A18]
    std::string string_pool;
    std::vector<TextToken> text_tokens;
    std::vector<OperandSymbol> operand_symbols;
    std::vector<CatalogLookup> catalog_lookups; // in the order the compiler made them
    std::vector<WordUse> word_uses;             // in the order the compiler read them
    // Effect handles are stable 1-based integers, separate from text-pool offsets.
    // The presentation consumer resolves these names in its mounted effect scene.
    std::vector<std::string> effect_names;
    std::vector<std::string> sound_names; // rebased nonzero SOUNDSET handles
    int event_count = 0;              // includes nested IF/event rules
    // The block depth each event was opened at: the IF/ELSEIF byte
    // [orig: byte_C67E18]. previous/chain/reset walk it backward.
    std::vector<uint16_t> event_depths;
    uint32_t loop_count = 0;
    std::vector<std::string> source_names;
    std::vector<InstructionSource> instruction_sources;
    std::vector<Diagnostic> diagnostics;

    bool ok() const {
        for (const Diagnostic &d : diagnostics) {
            if (d.error) return false;
        }
        return true;
    }
    // The text retail's debug overlay would show: the first diagnostic as
    // "file (line) message". [orig: Script_SetCompileError @0x4EE7C0]
    std::string first_error() const {
        if (diagnostics.empty()) return std::string();
        const Diagnostic &d = diagnostics.front();
        const std::string file = d.source < source_names.size() ? source_names[d.source] : std::string();
        return file + " (" + std::to_string(d.line) + ") " + d.message;
    }
    // The NUL-terminated string at a Text operand's offset.
    std::string text_at(uint32_t offset) const {
        if (offset >= string_pool.size()) return std::string();
        return std::string(string_pool.c_str() + offset);
    }
};

} // namespace opennova::wac
