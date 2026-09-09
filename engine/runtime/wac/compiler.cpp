#include <runtime/wac/compiler.h>
#include <runtime/particle/effect_catalog_names.h>
#include <runtime/audio/oneshot_play.h>

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>

#include <formats/wac/bytecode.h>
#include <formats/wac/command.h>
#include <formats/wac/lexer.h>
#include <formats/wac/parser.h>
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

    // Resolve an argument to an operand reference word. [orig: WacScript_ResolveParameter.]
    uint32_t resolve(const Arg &arg, ParamType type, int line, const CommandDef *def = nullptr) {
        const std::string &t = arg.text;

        // Declarations occupy the second half of the shared mission bank.
        // ARRAY follows the same scalar address path in this retail compiler.
        // [orig: Script_Compile @0x4F31F0; resolver @0x4F2940]
        for (size_t i = 0; !arg.is_string && i < variables_.size(); ++i) {
            if (ieq(t, variables_[i].c_str()))
                return encode_operand(OperandKind::MissionVar, uint32_t(i + 256));
        }
        for (size_t i = 0; !arg.is_string && i < event_names_.size(); ++i) {
            if (!event_names_[i].empty() && ieq(t, event_names_[i].c_str())) {
                if (type == ParamType::IfName)
                    return encode_operand(OperandKind::Pool, push_pool(int32_t(i)));
                // An event name is NULL for expectedType 27. [orig: @0x4f2a5e]
                if (type == ParamType::Variable) return unresolved_argument(def, line);
                return encode_operand(OperandKind::EventFired, uint32_t(i));
            }
        }
        if (type == ParamType::IfName) {
            warn(line, "unknown event '" + t + "'");
            return encode_operand(OperandKind::Pool, push_pool(-1));
        }
        // A quoted token matches no table or prefix, and expectedType 27 then
        // resolves to NULL. [orig: @0x4f2b7e]
        if (arg.is_string && type == ParamType::Variable) return unresolved_argument(def, line);

        // String / symbolic-asset params -> string pool, referenced as a pool value.
        bool string_like = (arg.is_string && type != ParamType::Group && type != ParamType::Anim && type != ParamType::Ammo && type != ParamType::Fx && type != ParamType::SoundSet && type != ParamType::Face) || type == ParamType::Text ||
                           type == ParamType::Filename ||
                           type == ParamType::TextToken;
        if (string_like) {
            int si = intern_string(t);
            return encode_operand(OperandKind::Pool, push_pool(si));
        }

        // Variable lvalue/rvalue: V# mission, G# global, M# music.
        if (!t.empty() && (t[0] == 'V' || t[0] == 'v') && all_digits(std::string_view(t).substr(1))) {
            int idx = std::atoi(t.c_str() + 1);
            if (idx >= 256) { warn(line, "V# too big"); idx = 255; }
            return encode_operand(OperandKind::MissionVar, idx);
        }
        if (!t.empty() && (t[0] == 'G' || t[0] == 'g') && t.size() > 1 &&
            std::isdigit(static_cast<unsigned char>(t[1]))) {
            int idx = std::atoi(t.c_str() + 1);
            if (idx >= world::ScriptVarStore::kGlobalVars) { warn(line, "G# too big"); idx = world::ScriptVarStore::kGlobalVars - 1; }
            return encode_operand(OperandKind::GlobalVar, idx);
        }
        if (!t.empty() && (t[0] == 'M' || t[0] == 'm') && t.size() > 1 &&
            std::isdigit(static_cast<unsigned char>(t[1]))) {
            int idx = std::atoi(t.c_str() + 1);
            return encode_operand(OperandKind::MusicVar, idx);
        }
        const int named_value = builtin_id(t);
        if (type == ParamType::Variable) {
            // Every row of the table @0x82EEF0 resolves to its mutable dword
            // regardless of the expected type, so each is an lvalue; a write
            // to a cached row lands on the cached word until the next bytecode
            // execution refreshes it. [orig: WacScript_ResolveParameter
            // @0x4f2a92..0x4f2a9f] Anything else is NULL for expectedType 27
            // [orig: @0x4f2b7e], the compile error + scratch-sink pair.
            if (named_value >= 0)
                return encode_operand(OperandKind::Builtin, static_cast<uint32_t>(named_value));
            return unresolved_argument(def, line);
        }

        // Named engine values (health/ticks/humans/accuracyspread/...).
        if (named_value >= 0) {
            return encode_operand(OperandKind::Builtin,
                                  static_cast<uint32_t>(named_value));
        }

        // Animation symbols resolve to the retail numeric state table, even
        // when used as an ordinary value. They are not string-pool indices.
        // [orig: WacScript_ResolveParameter @0x4F2920 -> AnimMap_FindSlotByName]
        const bool anim_prefix = starts_with_ci(t, "ANIM_");
        const bool numeric = !t.empty() &&
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

        // [orig: WacScript_ResolveParameter @0x4F2920 -> AmmoDef_LookupByName]
        // AMMO_ is a type prefix. The value is the ammo.def table index,
        // including when stored in a variable before a later fire command.
        // The null row (index zero) is not a successful name resolution.
        const bool ammo_prefix = starts_with_ci(t, "AMMO_");
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

        // [orig: WacScript_ResolveParameter @0x4F2940 -> @0x5F7310]
        // FX literals, including numeric-looking names, bind at compile time.
        // Variable operands were resolved above and carry the actual handle.
        const bool fx_prefix = starts_with_ci(t, "FX_");
        if (fx_prefix || type == ParamType::Fx) {
            const std::string name = fx_prefix ? t.substr(3) : t;
            const particle::EffectHandle handle = env_.effects ? env_.effects->intern(name)
                                                               : particle::EffectHandle{};
            if (!handle) {
                prog_.diagnostics.push_back({line, 0, "unknown FX '" + t + "'", true});
            }
            return encode_operand(OperandKind::Pool, push_pool(int32_t(handle.value)));
        }

        // [orig: WacScript_ResolveParameter @0x4F2940, expectedType 19]
        // SOUNDSET is an asset reference; variables carry the resolved handle,
        // and numeric literals name sets rather than bypassing resolution.
        const bool sound_prefix = starts_with_ci(t, "SS_");
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

        // FACE literals bind to the nine expression rows. Variables above
        // carry already-resolved values; numeric-looking literals still name
        // expressions and report Unknown FACE.
        // [orig: WacScript_ResolveParameter @0x4F2920 -> AnimState_FindByName @0x5800B0]
        const bool face_prefix = starts_with_ci(t, "FACE_");
        if (face_prefix || type == ParamType::Face) {
            const int index = world::facial_expression_index(face_prefix ? t.substr(5) : t);
            if (index < 0)
                prog_.diagnostics.push_back({line, 0, "unknown FACE '" + t + "'", true});
            return encode_operand(OperandKind::Pool, push_pool(index));
        }

        // Symbolic asset prefixes -> string pool.
        if (starts_with_ci(t, "TT_")) {
            int si = intern_string(t);
            return encode_operand(OperandKind::Pool, push_pool(si));
        }

        // Entity SSN constants bind once when the VM first receives its World.
        // Subsequent reads and variable aliases carry the packed entity handle.
        if (starts_with_ci(t, "SSN_")) {
            int net = std::atoi(t.c_str() + 4);
            return encode_operand(OperandKind::EntitySsn, push_pool(net));
        }
        // [orig: WacScript_ResolveParameter @0x4F2940] Named WAC groups
        // never select entities by their BMS commandGroup field.
        if (starts_with_ci(t, "G_") || type == ParamType::Group) {
            const std::string_view name = starts_with_ci(t, "G_") ? std::string_view(t).substr(2) : t;
            int group = env_.registry ? env_.registry->script_group_index(name)
                    : world::EntityRegistry::default_script_group_index(name);
            if (group < 0) { warn(line, "unknown group '" + std::string(name) + "'"); group = 0; }
            return encode_operand(OperandKind::Pool, push_pool(group));
        }

        // HH:MM time literal.
        if (t.find(':') != std::string::npos && std::isdigit(static_cast<unsigned char>(t[0]))) {
            int colon = static_cast<int>(t.find(':'));
            int h = std::atoi(t.substr(0, colon).c_str());
            int m = std::atoi(t.c_str() + colon + 1);
            int32_t v = h * 60 + m;
            if (arg.negate) v = -v;
            return encode_operand(OperandKind::Pool, push_pool(v));
        }

        // [orig: WacScript_ResolveParameter @0x4F2D0A..0x4F2D8C]
        // FogDist/MoveFog dispatches also use this resolver.
        // [orig: Script_Compile @0x4F4167 / @0x4F42C5]
        // Distance literals and M suffixes use Q16; F uses the retail 21501
        // factor. Named variables returned above already contain raw words
        // and are never rescaled at a distance-typed call site.
        if (!t.empty() && (std::isdigit(static_cast<unsigned char>(t[0])) || t[0] == '.' || t[0] == '-')) {
            double d = std::atof(t.c_str());
            if (arg.negate) d = -d;
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
            return encode_operand(type == ParamType::Ssn ? OperandKind::EntitySsn : OperandKind::Pool,
                                  push_pool(value));
        }

        // A token that matches no table, prefix or numeric form resolves to
        // NULL [orig: @0x4f2a62], the compile error + scratch-sink pair.
        return unresolved_argument(def, line);
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
        prog_.instruction_sources.push_back({static_cast<uint32_t>(prog_.code.size()), call.source_index, call.line});
        emit(encode_call(static_cast<uint16_t>(idx), fold, /*push=*/false, negate));
        for (int i = 0; i < def.argc; ++i) {
            ParamType pt = def.params[i];
            if (i < static_cast<int>(call.args.size())) {
                emit(resolve(call.args[i], pt, call.line, &def));
            } else {
                emit(encode_operand(OperandKind::Pool, push_pool(0)));
            }
        }
    }

    static Op expression_fold(const Expr &e) {
        if (e.kind == Expr::And || e.op == "and" || e.op == "&&") return Op::FoldAnd;
        if (e.kind == Expr::Or || e.op == "or" || e.op == "||") return Op::FoldOr;
        if (e.kind == Expr::Xor || e.op == "xor" || e.op == "!=" || e.op == "<>" || e.op == "~=") return Op::FoldNe;
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

    void compile_cond(const Expr &e, Op fold) {
        if (e.kind == Expr::Leaf) {
            emit_call(e.call, fold, false);
            return;
        }
        if (e.kind == Expr::Not && !e.kids.empty() && e.kids[0].kind == Expr::Leaf) {
            emit_call(e.kids[0].call, fold, true);
            return;
        }
        if (e.kids.empty()) return;
        const size_t first = prog_.code.size();
        compile_cond(e.kids[0], Op::Call);
        const bool negate_group = e.kind == Expr::Not;
        if (!negate_group) {
            const Op inner = expression_fold(e);
            for (size_t i = 1; i < e.kids.size(); ++i) compile_cond(e.kids[i], inner);
        }
        if ((fold != Op::Call || negate_group) && first < prog_.code.size()) {
            // Retail folds the saved byte INTO the new accumulator. For a
            // grouped RHS this reverses subtraction/comparison operands. NOT
            // is part of the pop operand and negates that saved byte.
            // [orig: Script_Compile @0x4F31F0; VM @0x4F5BDF]
            prog_.code[first] |= kPushBit;
            emit(encode_instr(Op::PopExpr, static_cast<uint32_t>(fold) | (negate_group ? 0x80u : 0)));
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
            if (s.kind == Stmt::Assignment) {
                emit(encode_instr(Op::StoreVar, 0));
                emit(resolve(s.assignment_target, ParamType::Variable, s.call.line));
            }
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
                const uint32_t ref = resolve(s.block_argument, ParamType::Group, s.call.line);
                if (operand_kind(ref) != OperandKind::Pool) {
                    prog_.diagnostics.push_back({s.call.line, 0, "GLOOP requires a constant group", true});
                    return;
                }
                group = static_cast<uint32_t>(prog_.operands[operand_index(ref)]);
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
