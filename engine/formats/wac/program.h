// Compiled WAC program: the faithful bytecode stream plus the rebased operand /
// string pools and the diagnostics gathered during compilation.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace opennova::mus { struct MusGlobals; }

namespace opennova::wac {

// One compile error in retail's wording. Retail keeps only the first one
// ("%s (%d) %s" into byte_C6EB30, Script_SetCompileError @0x4EE7C0), clears
// it before the load's compiles (WacScript_InitAndLoad @0x4F926A) and a
// console compile (WacScript_ExecuteConsoleCommand @0x4F6D3F), shows it only
// on the script debug overlay (Debug_DrawScriptState @0x4F652A) and never
// refuses the program. `error` marks the lookups that miss in OUR catalogs
// (FX, SOUNDSET, AMMO), which a strict host treats as unsafe to run.
struct Diagnostic {
    int line = 0;
    int col = 0;
    std::string message;
    bool error = false;
    uint32_t source = 0; // Program::source_names index
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
    int error_count() const {
        int n = 0;
        for (const Diagnostic &d : diagnostics) {
            if (d.error) ++n;
        }
        return n;
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
