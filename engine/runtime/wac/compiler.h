// WAC compiler: a structural translation of the retail compiler.
//
// Script_Compile @0x4F31F0 makes ONE pass over the source: an inline
// tokenizer, a one-character operator lookahead, a flat state machine (the
// pending operator and NOT, the push flag, a 16-frame paren stack, the
// parameter queue, the IF/DO/LOOP block stack) and direct emission into the
// shared bytecode buffer. WacScript_ResolveParameter @0x4F2920 turns one token
// into an operand. The operand words are the rebased tagged references of
// bytecode.h; the instruction words are the retail encoding.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <formats/wac/program.h>

namespace opennova::world {
class EntityRegistry;
struct AmmoTable;
}

namespace opennova::particle { class EffectCatalogNames; }
namespace opennova::audio { class SoundSetIndex; }

namespace opennova::wac {

struct CompileEnv {
    // Group names resolve through the world's WAC group table; without one the
    // seven default groups answer. Authored SSNs report "Unknown SSN" only when
    // a registry is given.
    opennova::world::EntityRegistry *registry = nullptr;
    const opennova::audio::SoundSetIndex *sounds = nullptr;
    // The mounted catalog's names; FX literals intern compile-time handles here.
    opennova::particle::EffectCatalogNames *effects = nullptr;
    const opennova::world::AmmoTable *ammo = nullptr; // literal AMMO bindings at compilation
    // A TextToken key's text: the mission text's entry, else the game text's,
    // else nullopt for the one "" string every missing key shares. Absent
    // means no mission text is loaded, so every key misses.
    // [orig: MissionText_GetStringByKeyOrGameText @0x51ECD0]
    std::function<std::optional<std::string>(const std::string &key)> text_token;
    std::vector<std::string> source_names; // parallel to compile_program's source texts
    std::function<bool(const std::string &, std::string &)> load_source; // RUN's mounted-file reader
    std::shared_ptr<opennova::mus::MusGlobals> music_globals;
    // The dword behind a variable, event or engine operand while the compile
    // runs, before the load resets anything: a GLOOP operand ORs it into the
    // GROUP word. Absent answers 0. [orig: Script_Compile @0x4F368A..0x4F3693]
    std::function<uint32_t(uint32_t ref)> load_dword;
};

// One source compiled as a whole program (terminator appended).
Program compile_source(std::string_view source, const CompileEnv &env);

// game.wac -> server.wac -> <mission>.wac compiled into one buffer: each file
// is its own Script_Compile call, the event, declaration, value and string
// tables and the first-error text are shared, and one terminator follows the
// last file. [orig: WacScript_InitAndLoad @0x4F91F0 (the compiles
// @0x4F94A8 / @0x4F950E / @0x4F9597, the terminator @0x4F95A9)]
Program compile_program(const std::vector<std::string> &sources, const CompileEnv &env);

} // namespace opennova::wac
