#include <runtime/wac/compiler.h>
#include <runtime/particle/effect_catalog_names.h>
#include <runtime/audio/oneshot_play.h>

#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <string>

#include <formats/wac/bytecode.h>
#include <formats/wac/command.h>
#include <formats/wac/lexer.h>
#include <formats/wac/parser.h>
#include <runtime/world/entity_commands.h>
#include <runtime/world/entity_registry.h>
#include <runtime/world/infantry.h>
#include <runtime/world/facial_animation.h>
#include <runtime/world/ammo_table.h>
#include <runtime/world/var_store.h>

#include <base/io/strutil.h>

namespace opennova::wac {
namespace {

bool ieq(std::string_view a, const char *b) { return opennova::strutil::iequals(a, b); }

bool all_digits(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

bool starts_with_ci(const std::string &s, const char *prefix) {
    size_t i = 0;
    for (; prefix[i]; ++i) {
        if (i >= s.size() ||
            std::tolower(static_cast<unsigned char>(s[i])) !=
                std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

// The 24-row named-value table @0x82EEF0 (count 0x18 @0x82F130), walked with
// stricmp by WacScript_ResolveParameter's third lookup leg.
int builtin_id(const std::string &name) {
    if (ieq(name, "ticks")) return static_cast<int>(Builtin::Ticks);
    if (ieq(name, "result")) return static_cast<int>(Builtin::Result);
    if (ieq(name, "SquadSSN")) return static_cast<int>(Builtin::SquadSSN);
    if (ieq(name, "SquadWho")) return static_cast<int>(Builtin::SquadWho);
    if (ieq(name, "RND")) return static_cast<int>(Builtin::RandomResult);
    if (ieq(name, "auto") || ieq(name, "player") || ieq(name, "item")) return static_cast<int>(Builtin::AutoItem);
    if (ieq(name, "health")) return static_cast<int>(Builtin::Health);
    if (ieq(name, "wind")) return static_cast<int>(Builtin::Wind);
    if (ieq(name, "mana")) return static_cast<int>(Builtin::Mana);
    if (ieq(name, "CurTOD")) return static_cast<int>(Builtin::CurTOD);
    if (ieq(name, "breathtime")) return static_cast<int>(Builtin::Breathtime);
    if (ieq(name, "autogain")) return static_cast<int>(Builtin::Autogain);
    // Round-outcome names from the named-value table @0x82EEF0 (case-insensitive,
    // like every entry — the resolver walks the table with stricmp).
    if (ieq(name, "bluekills")) return static_cast<int>(Builtin::Bluekills);
    if (ieq(name, "greenkills")) return static_cast<int>(Builtin::Greenkills);
    if (ieq(name, "humans")) return static_cast<int>(Builtin::Humans);
    if (ieq(name, "GameOver")) return static_cast<int>(Builtin::GameOver);
    if (ieq(name, "WinVar")) return static_cast<int>(Builtin::WinVar);
    if (ieq(name, "LoseVar")) return static_cast<int>(Builtin::LoseVar);
    if (ieq(name, "accuracyspread")) return static_cast<int>(Builtin::AccuracySpread);
    if (ieq(name, "fallmps")) return static_cast<int>(Builtin::Fallmps);
	if (ieq(name, "seatbelt"))
		return static_cast<int>(Builtin::Seatbelt);
	if (ieq(name, "night")) return static_cast<int>(Builtin::Night);
    return -1;
}

void stamp_source(Expr &expr, uint32_t source) {
    expr.call.source_index = source;
    for (Expr &child : expr.kids) stamp_source(child, source);
}

void stamp_source(Stmt &stmt, uint32_t source) {
    stmt.call.source_index = source;
    stamp_source(stmt.cond, source);
    for (Stmt &child : stmt.body) stamp_source(child, source);
    for (ElseIf &branch : stmt.elifs) {
        stamp_source(branch.cond, source);
        for (Stmt &child : branch.body) stamp_source(child, source);
    }
    for (Stmt &child : stmt.else_body) stamp_source(child, source);
    for (auto &body : stmt.next_bodies) for (Stmt &child : body) stamp_source(child, source);
}

class Compiler {
public:
    explicit Compiler(const CompileEnv &env) : env_(env) {
        prog_.source_names = env.source_names;
        if (prog_.source_names.empty()) prog_.source_names.emplace_back();
    }

    Program compile(const std::vector<Stmt> &stmts) {
        for (const Stmt &s : stmts) {
            compile_top(s);
        }
        prog_.code.push_back(kProgramTerminator);
        prog_.event_count = event_counter_;
        // Retain the mounted table's existing handles too: a replacement
        // script may consume an FX handle already held in a mission variable.
        if (env_.effects) prog_.effect_names = env_.effects->interned_names();
        if (env_.sounds) prog_.sound_names = env_.sounds->names();
        return std::move(prog_);
    }

private:
    const CompileEnv &env_;
    Program prog_;
    int event_counter_ = 0;
    int include_depth_ = 0;
    int group_loop_depth_ = 0;
    std::vector<std::string> variables_;
    std::vector<std::string> event_names_;

    void warn(int line, const std::string &msg) {
        prog_.diagnostics.push_back(Diagnostic{line, 0, msg, false});
    }

    size_t emit(uint32_t word) {
        prog_.code.push_back(word);
        return prog_.code.size() - 1;
    }
    void patch_target(size_t instr_pos, size_t target_index) {
        uint32_t w = prog_.code[instr_pos];
        w = (w & ~kOperand24Mask) | (static_cast<uint32_t>(target_index) & kOperand24Mask);
        prog_.code[instr_pos] = w;
    }

    int push_pool(int32_t value) {
        for (size_t i = 0; i < prog_.operands.size(); ++i) {
            if (prog_.operands[i] == value) return static_cast<int>(i);
        }
        prog_.operands.push_back(value);
        return static_cast<int>(prog_.operands.size() - 1);
    }
    int intern_string(const std::string &s) {
        for (size_t i = 0; i < prog_.strings.size(); ++i) {
            if (prog_.strings[i] == s) return static_cast<int>(i);
        }
        prog_.strings.push_back(s);
        return static_cast<int>(prog_.strings.size() - 1);
    }

    // [orig: WacScript_FormatActionParameters @0x4EFC20] "  name (type, type)".
    static std::string action_signature(const CommandDef &def) {
        std::string text = "  ";
        text += def.name;
        text += " (";
        for (int i = 0; i < 4; ++i) {
            if (def.params[i] == ParamType::Null) continue;
            if (i) text += ", ";
            text += param_type_name(def.params[i]);
        }
        text += ")";
        return text;
    }

    // [orig: Script_Compile @0x4F3AB2..0x4F3AE2] An argument the resolver
    // returns NULL for logs the command's signature as the compile error
    // (Script_SetCompileError @0x4EE7C0, a first-error buffer only the script
    // debug overlay @0x4f652a and the console @0x4f6d3f read) and its operand
    // slot points at the shared scratch dword &dword_C6EAEC (Builtin::Scratch,
    // zeroed at every bytecode entry). Compilation continues and
    // WacScript_InitAndLoad runs the program regardless [orig: @0x4f926a
    // clears the buffer, @0x4f976b executes], so the diagnostic is not
    // fatal: the lenient loader keeps the script. V0 is never an implicit
    // target.
    uint32_t unresolved_argument(const CommandDef *def, int line) {
        prog_.diagnostics.push_back({line, 0,
                def != nullptr ? action_signature(*def) : std::string("unresolved variable"), false});
        return encode_operand(OperandKind::Builtin, static_cast<uint32_t>(Builtin::Scratch));
    }

    // Resolve an argument to an operand reference word, or the scratch sink
    // plus the signature diagnostic when the resolver's answer is NULL.
    uint32_t resolve(const Arg &arg, ParamType type, int line, const CommandDef *def = nullptr) {
        if (const std::optional<uint32_t> ref = try_resolve(arg, type, line)) return *ref;
        return unresolved_argument(def, line);
    }

    // [orig: WacScript_ResolveParameter @0x4f2920, the SSN leg
    //  @0x4f2c94..0x4f2eed] The SSN leg takes every token an Ssn slot receives
    // (expectedType 11) and every SSN_-prefixed token: atol (0 for a name, or
    // a quoted token, whose buffer keeps its quote @0x4f3338) ->
    // EntityPool_FindByNetId; a 0xFFFF miss logs "Unknown SSN"
    // (Script_SetCompileError @0x4f2edf) and the handle still lands in the
    // operand pool, so an Ssn slot is never the NULL leg. The port binds the
    // net id when the VM first meets its world (WacVm::execute) and asks the
    // compile-time registry, when the embedder passes one, the question
    // retail's pool answered. The kLocalPlayerSsn (10000) exemption is a
    // port seam, not a retail rule: retail's leg has no alias, and
    // EntityPool_FindByNetId @0x4f0a20 keys on GamePlayerEntity+0x7C (DcbId),
    // which the JO player spawn [orig: Entity_SpawnFromAnimSlotProperty
    // @0x43c390] leaves at 0, so retail reports Unknown SSN for 10000 unless
    // an authored entity carries that DcbId. The exemption mirrors
    // EntityCommands::resolve_ssn, which honours the dfx2med authoring
    // convention (the local player is SSN 10000) that mission scripts are
    // written against.
    uint32_t ssn_operand(int32_t net, int line) {
        if (env_.registry != nullptr && uint16_t(net) != world::EntityCommands::kLocalPlayerSsn &&
                !env_.registry->find_by_net_id(uint16_t(net)).valid())
            warn(line, "Unknown SSN");
        return encode_operand(OperandKind::EntitySsn, push_pool(net));
    }

    // The resolver proper; std::nullopt is retail's NULL return. Retail's
    // token buffer keeps a quoted token's opening quote [orig: Script_Compile
    // @0x4f3338] and uppercases a bare one [orig: @0x4f3418..0x4f341d], so
    // every stricmp, prefix and first-character test misses a quoted token:
    // only a slot's type-gated leg consumes one, and a slot with no such leg
    // (Number, Value, Distance, Hour, ...) reaches the numeric test, which
    // the quote fails into the NULL return [orig: @0x4f2d01 -> @0x4f2a62].
    // `bare` is that quote gate.
    // [orig: WacScript_ResolveParameter @0x4f2920]
    std::optional<uint32_t> try_resolve(const Arg &arg, ParamType type, int line) {
        const std::string &t = arg.text;
        const bool bare = !arg.is_string;

        // Table 0: the declared variables, the second half of the shared
        // mission bank. ARRAY follows the same scalar address path in this
        // retail compiler. [orig: Script_Compile @0x4F31F0; the resolver's
        // first table @0x4f2970..0x4f2a3c]
        for (size_t i = 0; bare && i < variables_.size(); ++i) {
            if (ieq(t, variables_[i].c_str()))
                return encode_operand(OperandKind::MissionVar, uint32_t(i + 256));
        }
        // Table 1: the event names. An IfName slot takes the index as a pool
        // value [orig: @0x4f2a6c]; expectedType 27 is NULL [orig: @0x4f2a5e];
        // every other slot reads the fired dword [orig: @0x4f2a8a]. A miss
        // falls through to every leg below [orig: @0x4f29c2 -> loc_4F29C4],
        // so an IfName token naming no event is whatever those make of it,
        // and a bare name ends in the NULL return.
        for (size_t i = 0; bare && i < event_names_.size(); ++i) {
            if (!event_names_[i].empty() && ieq(t, event_names_[i].c_str())) {
                if (type == ParamType::IfName)
                    return encode_operand(OperandKind::Pool, push_pool(int32_t(i)));
                if (type == ParamType::Variable) return std::nullopt;
                return encode_operand(OperandKind::EventFired, uint32_t(i));
            }
        }
        // Table 2: the named engine values (health/ticks/humans/...). Every
        // row of the table @0x82EEF0 resolves to its mutable dword regardless
        // of the expected type [orig: @0x4f2a92..0x4f2a9f], so each is an
        // lvalue for expectedType 27 too; a write to a cached row lands on
        // the cached word until the next bytecode execution refreshes it.
        if (bare) {
            const int named_value = builtin_id(t);
            if (named_value >= 0)
                return encode_operand(OperandKind::Builtin, static_cast<uint32_t>(named_value));
        }

        // M# music, V# mission, G# global: a letter then a digit, in retail's
        // order [orig: @0x4f29f8 (M), @0x4f2aa7 (V), @0x4f2b0e (G)].
        if (bare && !t.empty() && (t[0] == 'M' || t[0] == 'm') && t.size() > 1 &&
            std::isdigit(static_cast<unsigned char>(t[1]))) {
            int idx = std::atoi(t.c_str() + 1);
            return encode_operand(OperandKind::MusicVar, idx);
        }
        if (bare && !t.empty() && (t[0] == 'V' || t[0] == 'v') && all_digits(std::string_view(t).substr(1))) {
            int idx = std::atoi(t.c_str() + 1);
            if (idx >= 256) { warn(line, "V# too big"); idx = 255; }
            return encode_operand(OperandKind::MissionVar, idx);
        }
        if (bare && !t.empty() && (t[0] == 'G' || t[0] == 'g') && t.size() > 1 &&
            std::isdigit(static_cast<unsigned char>(t[1]))) {
            int idx = std::atoi(t.c_str() + 1);
            if (idx >= world::ScriptVarStore::kGlobalVars) { warn(line, "G# too big"); idx = world::ScriptVarStore::kGlobalVars - 1; }
            return encode_operand(OperandKind::GlobalVar, idx);
        }

        // Anything else is NULL for expectedType 27 [orig: @0x4f2b7e], the
        // compile error + scratch-sink pair.
        if (type == ParamType::Variable) return std::nullopt;

        // From here the legs run in retail's order, each taken by its prefix
        // (never on a quoted token) or by the slot's expected type: G_/12
        // @0x4f2b8d, FX_/22 @0x4f2bb4, FACE_/21 @0x4f2be7, SS_/19 @0x4f2c0e,
        // TT_/20 @0x4f2c34, ANIM_/24 @0x4f2c6a, SSN_/11 @0x4f2c94, AMMO_/23
        // @0x4f2cc5, the 17/18 string copy @0x4f2ce9, then the numeric test
        // @0x4f2d01.

        // [orig: WacScript_ResolveParameter @0x4F2940] Named WAC groups
        // never select entities by their BMS commandGroup field.
        const bool group_prefix = bare && starts_with_ci(t, "G_");
        if (group_prefix || type == ParamType::Group) {
            const std::string_view name = group_prefix ? std::string_view(t).substr(2) : t;
            int group = env_.registry ? env_.registry->script_group_index(name)
                    : world::EntityRegistry::default_script_group_index(name);
            // The miss is retail's first-error 'Unknown Group' with the pool
            // slot holding 0 [orig: @0x4f30fc -> @0x4f310a].
            if (group < 0) { warn(line, "Unknown Group '" + std::string(name) + "'"); group = 0; }
            return encode_operand(OperandKind::Pool, push_pool(group));
        }

        // [orig: WacScript_ResolveParameter @0x4F2940 -> @0x5F7310]
        // FX literals, including numeric-looking names, bind at compile time.
        // Variable operands were resolved above and carry the actual handle.
        const bool fx_prefix = bare && starts_with_ci(t, "FX_");
        if (fx_prefix || type == ParamType::Fx) {
            const std::string name = fx_prefix ? t.substr(3) : t;
            const particle::EffectHandle handle = env_.effects ? env_.effects->intern(name)
                                                               : particle::EffectHandle{};
            if (!handle) {
                prog_.diagnostics.push_back({line, 0, "unknown FX '" + t + "'", true});
            }
            return encode_operand(OperandKind::Pool, push_pool(int32_t(handle.value)));
        }

        // FACE literals bind to the nine expression rows. Variables above
        // carry already-resolved values; numeric-looking literals still name
        // expressions and report Unknown FACE.
        // [orig: WacScript_ResolveParameter @0x4F2920 -> AnimState_FindByName @0x5800B0]
        const bool face_prefix = bare && starts_with_ci(t, "FACE_");
        if (face_prefix || type == ParamType::Face) {
            const int index = world::facial_expression_index(face_prefix ? t.substr(5) : t);
            if (index < 0)
                prog_.diagnostics.push_back({line, 0, "unknown FACE '" + t + "'", true});
            return encode_operand(OperandKind::Pool, push_pool(index));
        }

        // [orig: WacScript_ResolveParameter @0x4F2940, expectedType 19]
        // SOUNDSET is an asset reference; variables carry the resolved handle,
        // and numeric literals name sets rather than bypassing resolution.
        const bool sound_prefix = bare && starts_with_ci(t, "SS_");
        if (sound_prefix || type == ParamType::SoundSet) {
            const std::string name = sound_prefix ? t.substr(3) : t;
            int32_t handle = 0;
            if (env_.sounds) {
                const auto &names = env_.sounds->names();
                for (size_t i = 0; i < names.size(); ++i)
                    if (ieq(name, names[i].c_str())) { handle = int32_t(i + 1); break; }
            }
            if (handle == 0)
                prog_.diagnostics.push_back({line, 0, "unknown SOUNDSET '" + t + "'", true});
            return encode_operand(OperandKind::Pool, push_pool(handle));
        }

        // Text-tool tokens -> string pool. Retail stores the mission-text
        // pointer the key resolves to [orig: @0x4f2f9e]; the port keeps the
        // key and resolves the text at execution.
        if ((bare && starts_with_ci(t, "TT_")) || type == ParamType::TextToken) {
            int si = intern_string(t);
            return encode_operand(OperandKind::Pool, push_pool(si));
        }

        // Animation symbols resolve to the retail numeric state table, even
        // when used as an ordinary value. They are not string-pool indices.
        // [orig: WacScript_ResolveParameter @0x4F2920 -> AnimMap_FindSlotByName]
        const bool anim_prefix = bare && starts_with_ci(t, "ANIM_");
        const bool numeric = bare && !t.empty() &&
                (std::isdigit(static_cast<unsigned char>(t[0])) ||
                 t[0] == '-' || t[0] == '+' || t[0] == '.');
        if (anim_prefix || (type == ParamType::Anim && !numeric)) {
            const std::string name = anim_prefix ? t.substr(5) : t;
            for (int state = 0; state < world::kInfantryAnimStateCount; ++state)
                if (ieq(name, world::kInfantryAnimNames[state]))
                    return encode_operand(OperandKind::Pool, push_pool(state));
            prog_.diagnostics.push_back({line, 0, "unknown animation '" + t + "'", true});
            return encode_operand(OperandKind::Pool, push_pool(-1));
        }

        // Entity SSN constants bind once when the VM first receives its World;
        // subsequent reads and variable aliases carry the packed entity handle.
        // The leg sits after the ANIM test and before the AMMO one, as in
        // retail [orig: @0x4f2c94..0x4f2ca4 precedes @0x4f2cc5]: an AMMO_
        // token in an Ssn slot is atol'd (0) and looked up, and an SSN_ token
        // in an Ammo slot is this leg's. A quoted token reads 0.
        const bool ssn_prefix = bare && starts_with_ci(t, "SSN_");
        if (ssn_prefix || type == ParamType::Ssn) {
            int32_t net = bare ? std::atoi(t.c_str() + (ssn_prefix ? 4 : 0)) : 0;
            return ssn_operand(net, line);
        }

        // [orig: WacScript_ResolveParameter @0x4F2920 -> AmmoDef_LookupByName]
        // AMMO_ is a type prefix. The value is the ammo.def table index,
        // including when stored in a variable before a later fire command.
        // The null row (index zero) is not a successful name resolution.
        const bool ammo_prefix = bare && starts_with_ci(t, "AMMO_");
        if (ammo_prefix || type == ParamType::Ammo) {
            const std::string name = ammo_prefix ? t.substr(5) : t;
            int index = env_.ammo ? env_.ammo->index_of(name.c_str()) : -1;
            if (index <= 0 && env_.ammo)
                index = env_.ammo->index_of(("ammo_" + name).c_str());
            if (index <= 0) {
                prog_.diagnostics.push_back({line, 0, "unknown AMMO '" + t + "'", true});
                index = 0;
            }
            return encode_operand(OperandKind::Pool, push_pool(index));
        }

        // Text / Filename slots take the token, quoted or bare, as a
        // string-pool operand [orig: @0x4f2ce9..0x4f2e1c, expectedType 17/18;
        // the copy skips the opening quote @0x4f2db4]. No other slot does: a
        // quoted token anywhere else reaches the numeric test below and is
        // NULL there.
        if (type == ParamType::Text || type == ParamType::Filename) {
            int si = intern_string(t);
            return encode_operand(OperandKind::Pool, push_pool(si));
        }

        // HH:MM time literal.
        if (bare && t.find(':') != std::string::npos && (std::isdigit(static_cast<unsigned char>(t[0])) || t[0] == '-')) {
            int colon = static_cast<int>(t.find(':'));
            int h = std::atoi(t.substr(0, colon).c_str());
            int m = std::atoi(t.c_str() + colon + 1);
            int32_t v = h * 60 + m;
            return encode_operand(OperandKind::Pool, push_pool(v));
        }

        // [orig: WacScript_ResolveParameter @0x4F2D0A..0x4F2D8C]
        // FogDist/MoveFog dispatches also use this resolver.
        // [orig: Script_Compile @0x4F4167 / @0x4F42C5]
        // Distance literals and M suffixes use Q16; F uses the retail 21501
        // factor. Named variables returned above already contain raw words
        // and are never rescaled at a distance-typed call site.
        if (bare && !t.empty() && (std::isdigit(static_cast<unsigned char>(t[0])) || t[0] == '.' || t[0] == '-')) {
            double d = std::atof(t.c_str());
            const int suffix = std::toupper(static_cast<unsigned char>(t.back()));
            if (suffix == 'F') d *= 21501.0;
            else if (suffix == 'M' || type == ParamType::Distance) d *= 65536.0;
            else if (type == ParamType::Hour) d *= 60.0;
            // _ftol2_sse returns a signed 64-bit truncation; the operand stores
            // its low word. Invalid conversions yield the indefinite low zero.
            int32_t value = 0;
            if (std::isfinite(d) && d >= -9223372036854775808.0 &&
                    d < 9223372036854775808.0)
                value = static_cast<int32_t>(static_cast<uint32_t>(static_cast<int64_t>(d)));
            return encode_operand(OperandKind::Pool, push_pool(value));
        }

        // A token that matches no table, prefix or numeric form, a quoted
        // token outside a Text/Filename slot among them, resolves to NULL
        // [orig: @0x4f2d01 -> @0x4f2a62], the compile error + scratch-sink
        // pair.
        return std::nullopt;
    }

    void emit_call_word(int idx, Op fold, bool negate, const Call &call) {
        prog_.instruction_sources.push_back({static_cast<uint32_t>(prog_.code.size()), call.source_index, call.line});
        emit(encode_call(static_cast<uint16_t>(idx), fold, /*push=*/false, negate));
    }

    // Emit a single command call (condition leaf or action). `fold` controls how
    // the return value combines into the accumulator (Assign for the first term).
    void emit_call(Call call, Op fold, bool negate) {
        if (call.name == "$operand") {
            // Bare values preserve their dword for arithmetic and comparison.
            call.name = "load";
        }
        int idx = wac_command_index(call.name);
        if (idx < 0) {
            warn(call.line, "unknown command '" + call.name + "'");
            return;
        }
        const CommandDef &def = wac_commands()[idx];
        emit_call_word(idx, fold, negate, call);
        // Retail's argument loop owns ONE token buffer. A token the resolver
        // binds fills the slot and the tokenizer reads the next token
        // [orig: Script_Compile @0x4f3af2..0x4f3afd -> loc_4F32E0]; a token it
        // returns NULL for fills the slot with the scratch sink and re-enters
        // the loop with the SAME token [orig: @0x4f3ab2..0x4f3aed ->
        // loc_4F3990], which the next slot's expected type re-classifies
        // [orig: @0x4f3a71..0x4f3aaa]. The source cursor and the slot index
        // therefore move apart, and the tokens left once the slots are full
        // are statement-level tokens [orig: @0x4f3a76 -> loc_4F3B02].
        size_t cursor = 0;
        for (int slot = 0; slot < def.argc; ++slot) {
            if (cursor >= call.args.size()) {
                emit(encode_operand(OperandKind::Pool, push_pool(0)));
                continue;
            }
            const std::optional<uint32_t> ref = try_resolve(call.args[cursor], def.params[slot], call.line);
            if (ref) {
                emit(*ref);
                ++cursor;
            } else {
                emit(unresolved_argument(&def, call.line));
            }
        }
        stray_arguments(call, cursor);
    }

    // The statement-level classification of the tokens left over once a
    // call's slots are full [orig: Script_Compile's keyword default]: the
    // token is resolved as a bare value with expectedType 1
    // (@0x4f50f5..0x4f5108) and, with no `=` following (@0x4f511a), rewritten
    // to `load` (@0x4f5124..0x4f5136), whose CALL word takes the already
    // cleared operator state, an assign of the accumulator
    // (@0x4f5321..0x4f533d), and the value as its inline operand (@0x4f533f);
    // a token that is NULL there walks the action table (@0x4f5252..0x4f5282)
    // and starts a new call fed by the tokens after it; a token matching
    // nothing is the first-error "Unknown '<token>'" (@0x4f5293) and the
    // tokenizer moves on with nothing emitted (@0x4f52b4). A quoted token
    // keeps its quote in retail's buffer (@0x4f3338), so nothing matches it.
    void stray_arguments(const Call &call, size_t cursor) {
        while (cursor < call.args.size()) {
            const Arg &arg = call.args[cursor++];
            if (!arg.is_string) {
                if (const std::optional<uint32_t> ref = try_resolve(arg, ParamType::Value, call.line)) {
                    emit_call_word(wac_command_index("load"), Op::Call, false, call);
                    emit(*ref);
                    continue;
                }
                if (wac_command_index(arg.text) >= 0) {
                    Call rest;
                    rest.name = arg.text;
                    rest.args.assign(call.args.begin() + static_cast<std::ptrdiff_t>(cursor), call.args.end());
                    rest.line = call.line;
                    rest.source_index = call.source_index;
                    emit_call(std::move(rest), Op::Call, false);
                    return;
                }
            }
            warn(call.line, "Unknown '" + arg.text + "'");
        }
    }

    static Op expression_fold(const Expr &e) {
        if (e.op == "and" || e.op == "&&") return Op::FoldAnd;
        if (e.op == "or" || e.op == "||") return Op::FoldOr;
        if (e.op == "xor" || e.op == "!=" || e.op == "<>" || e.op == "~=") return Op::FoldNe;
        if (e.op == "+") return Op::FoldAdd;
        if (e.op == "-") return Op::FoldSub;
        if (e.op == "*") return Op::FoldMul;
        if (e.op == "/") return Op::FoldDiv;
        if (e.op == "%") return Op::FoldMod;
        if (e.op == "^") return Op::FoldPow;
        if (e.op == "==") return Op::FoldEq;
        if (e.op == "<") return Op::FoldLt;
        if (e.op == ">") return Op::FoldGt;
        if (e.op == "<=") return Op::FoldLe;
        if (e.op == ">=") return Op::FoldGe;
        return Op::Call;
    }

    void compile_cond(const Expr &e, Op = Op::Call) {
        if (e.kind == Expr::Sequence) {
            for (const Expr &step : e.kids) compile_cond(step);
        } else if (e.kind == Expr::Store) {
            emit(encode_instr(Op::StoreVar, 0));
            emit(resolve(e.call.args[0], ParamType::Variable, e.call.line));
        } else if (e.kind == Expr::Pop) {
            // [orig: Script_Compile @0x4F31F0; VM @0x4F5BDF]
            // Pop folds the saved BYTE into the new result, reversing the
            // operands of subtraction/division/comparison. NOT negates it.
            emit(encode_instr(Op::PopExpr, static_cast<uint32_t>(expression_fold(e)) | (e.negate ? 0x80u : 0)));
        } else {
            const size_t first = prog_.code.size();
            emit_call(e.call, expression_fold(e), e.negate);
            if (e.push && first < prog_.code.size()) prog_.code[first] |= kPushBit;
        }
    }

    void declare_variable(const Stmt &s) {
        const std::string name = s.call.name.substr(0, 18);
        bool used = builtin_id(name) >= 0;
        for (const auto &existing : variables_) used |= ieq(existing, name.c_str());
        for (const auto &existing : event_names_) used |= ieq(existing, name.c_str());
        if (used || variables_.size() >= 256) {
            warn(s.call.line, used ? "variable name already used" : "out of variable space");
            return;
        }
        variables_.push_back(name);
    }

    void compile_run(const Stmt &s) {
        // The literal counter admits two include levels; the third reports
        // "A run file can't run more files". [orig: Script_Compile @0x4F31F0]
        if (include_depth_ > 1) {
            prog_.diagnostics.push_back({s.call.line, 0, "RUN nesting limit exceeded", true});
            return;
        }
        std::string name = s.call.name;
        const size_t dot = name.find_last_of('.');
        if (dot != std::string::npos) name.resize(dot);
        name += ".wac";
        std::string source;
        if (!env_.load_source || !env_.load_source(name, source)) {
            prog_.diagnostics.push_back({s.call.line, 0, "unable to RUN '" + name + "'", true});
            return;
        }
        const uint32_t source_index = uint32_t(prog_.source_names.size());
        prog_.source_names.push_back(name);
        ParseResult parsed = parse(source);
        prog_.diagnostics.insert(prog_.diagnostics.end(), parsed.diagnostics.begin(), parsed.diagnostics.end());
        ++include_depth_;
        for (Stmt &child : parsed.statements) {
            stamp_source(child, source_index);
            compile_top(child);
        }
        --include_depth_;
    }

    void compile_top(const Stmt &s) {
        if (s.kind == Stmt::If) {
            compile_if(s);
        } else if (s.kind == Stmt::Action) {
            emit_call(s.call, Op::Call, false);
        } else if (s.kind == Stmt::Block) {
            compile_block(s, -1, 1);
        } else if (s.kind == Stmt::Declaration) {
            declare_variable(s);
        } else if (s.kind == Stmt::Run) {
            compile_run(s);
        } else if (s.kind == Stmt::Assignment || s.kind == Stmt::Expression) {
            compile_cond(s.cond, Op::Call);
        }
    }

    // Compile each IF/ELSEIF as its own event. Retail leaves the most recently
    // entered event current after nested bodies; END does not restore a parent.
    void compile_if(const Stmt &s, uint16_t depth = 1) {
        int e = event_counter_++;
        event_names_.push_back(s.event_name.substr(0, 18));
        prog_.event_depths.push_back(depth);
        emit(encode_instr(Op::EnterEvent, static_cast<uint32_t>(e)));
        compile_cond(s.cond, Op::Call); // accumulator = condition
        // THEN runs every true evaluation; ENTER/LEAVE run on edges.
        // [orig: Script_Compile @0x4F31F0; ExecuteBytecode @0x4F58B0]
        const Op branch = s.mode == IfMode::Enter ? Op::AndChain
                : s.mode == IfMode::Leave ? Op::OrChain : Op::MarkFired;
        size_t andchain = emit(encode_instr(branch, 0));
        compile_body(s.body, e, depth);
        bool has_else_chain = !s.elifs.empty() || s.has_else;
        if (has_else_chain) {
            size_t jmp = emit(encode_instr(Op::Jump, 0));
            patch_target(andchain, prog_.code.size()); // L_else
            compile_else_chain(s, e);
            patch_target(jmp, prog_.code.size());       // L_end
        } else {
            patch_target(andchain, prog_.code.size());  // L_end
        }
    }

    void compile_body(const std::vector<Stmt> &body, int parent_event, uint16_t depth) {
        for (const Stmt &st : body) {
            if (st.kind == Stmt::Action) {
                emit_call(st.call, Op::Call, false);
            } else if (st.kind == Stmt::If) {
                compile_if(st, depth + 1);
            } else if (st.kind == Stmt::Block) {
                compile_block(st, parent_event, depth + 1);
            } else if (st.kind == Stmt::Declaration || st.kind == Stmt::Assignment || st.kind == Stmt::Expression) {
                compile_top(st);
            } else if (st.kind == Stmt::Run) {
                prog_.diagnostics.push_back({st.call.line, 0, "RUN is not allowed inside blocks", true});
            }
        }
    }

    void compile_block(const Stmt &s, int parent_event, uint16_t depth) {
        if (s.block_kind == "ploop" || s.block_kind == "gloop") {
            if (group_loop_depth_ != 0) {
                prog_.diagnostics.push_back({s.call.line, 0, "No LOOP Nesting!", true});
                return;
            }
            if (!s.next_bodies.empty())
                prog_.diagnostics.push_back({s.call.line, 0, "NEXT requires a DO block", true});
            uint32_t group = 1;
            if (s.block_kind == "gloop") {
                // The operand resolves with expectedType 12: a token naming no
                // group is the non-fatal first-error 'Unknown Group' and the
                // pool slot holding 0, so the loop selects record 0 (the empty
                // group) and the script still runs. A token an earlier resolver
                // table claims (a declared variable, an event, a named value)
                // is ORed in as the dword behind that address at compile time;
                // the port takes group 0 for it, under D-WAC-6.
                // [orig: WacScript_ResolveParameter group leg @0x4f30a0..0x4f30fc
                //  -> pool slot 0 @0x4f310a; the `or [gloopPatch], [eax]`
                //  @0x4f368a..0x4f3693; VM opcode 0xA @0x4f5b11 selects record 0]
                const uint32_t ref = resolve(s.block_argument, ParamType::Group, s.call.line);
                if (operand_kind(ref) == OperandKind::Pool) {
                    group = static_cast<uint32_t>(prog_.operands[operand_index(ref)]);
                } else {
                    warn(s.call.line, "Unknown Group '" + s.block_argument.text + "'");
                    group = 0;
                }
            }
            // [orig: Script_Compile @0x4F31F0; PLOOP/GLOOP emit 0xA then 9]
            emit(encode_instr(Op::GroupIter, group));
            const size_t next = emit(encode_instr(Op::LocalPlayer, 0));
            ++group_loop_depth_;
            compile_body(s.body, parent_event, depth);
            --group_loop_depth_;
            emit(encode_instr(Op::Jump, static_cast<uint32_t>(next)));
            patch_target(next, prog_.code.size());
            return;
        }
        if (s.block_kind != "doseq" && s.block_kind != "dornd") {
            warn(0, "unsupported block '" + s.block_kind + "'");
            return;
        }
        // BOTH spellings emit opcode 4 in this retail compiler, including
        // DORND. Opcode 5 exists in the VM but is not emitted by this arm.
        // [orig: Script_Compile stores @0x4F4143 / @0x4F429D]
        const uint32_t loop = prog_.loop_count++;
        size_t branch = emit(encode_instr(Op::DoSeq, 0));
        emit((loop << 16) | uint16_t(s.next_bodies.size() + 1));
        compile_body(s.body, parent_event, depth);
        std::vector<size_t> ends;
        for (const auto &alternative : s.next_bodies) {
            ends.push_back(emit(encode_instr(Op::Jump, 0)));
            patch_target(branch, prog_.code.size());
            branch = emit(encode_instr(Op::NextDo, 0));
            emit(loop);
            compile_body(alternative, parent_event, depth);
        }
        patch_target(branch, prog_.code.size());
        for (size_t end : ends) patch_target(end, prog_.code.size());
    }

    void compile_else_chain(const Stmt &s, int parent_event) {
        if (!s.elifs.empty()) {
            // Desugar: first elseif becomes an IF whose else is the remaining chain.
            Stmt syn;
            syn.kind = Stmt::If;
            syn.event_name = s.elifs[0].event_name;
            syn.cond = s.elifs[0].cond;
            syn.mode = s.elifs[0].mode;
            syn.body = s.elifs[0].body;
            syn.elifs.assign(s.elifs.begin() + 1, s.elifs.end());
            syn.else_body = s.else_body;
            syn.has_else = s.has_else;
            compile_if(syn, prog_.event_depths[parent_event]);
        } else if (s.has_else) {
            compile_body(s.else_body, parent_event, prog_.event_depths[parent_event]);
        }
    }
};

} // namespace

Program compile(const std::vector<Stmt> &statements, const CompileEnv &env) {
    Compiler c(env);
    return c.compile(statements);
}

Program compile_source(std::string_view source, const CompileEnv &env) {
    ParseResult pr = parse(source);
    Program prog = compile(pr.statements, env);
    // Prepend parse diagnostics.
    prog.diagnostics.insert(prog.diagnostics.begin(), pr.diagnostics.begin(),
                            pr.diagnostics.end());
    return prog;
}

Program compile_program(const std::vector<std::string> &sources, const CompileEnv &env) {
    std::vector<Stmt> all;
    std::vector<Diagnostic> parse_diags;
    uint32_t source_index = 0;
    for (const std::string &src : sources) {
        ParseResult pr = parse(src);
        for (Stmt &s : pr.statements) {
            stamp_source(s, source_index);
            all.push_back(std::move(s));
        }
        ++source_index;
        for (Diagnostic &d : pr.diagnostics) parse_diags.push_back(std::move(d));
    }
    CompileEnv source_env = env;
    source_env.source_names.resize(sources.size());
    Program prog = compile(all, source_env);
    prog.diagnostics.insert(prog.diagnostics.begin(), parse_diags.begin(), parse_diags.end());
    return prog;
}

} // namespace opennova::wac
