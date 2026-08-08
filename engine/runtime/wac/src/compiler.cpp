#include "wac/compiler.h"

#include <cctype>
#include <cstdlib>
#include <string>

#include "wac/bytecode.h"
#include "wac/command.h"
#include "wac/lexer.h"
#include "wac/parser.h"
#include "world/entity_registry.h"
#include "world/var_store.h"

#include <io/strutil.h>

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

int builtin_id(const std::string &name) {
    if (ieq(name, "ticks")) return static_cast<int>(Builtin::Ticks);
    if (ieq(name, "result")) return static_cast<int>(Builtin::Result);
    if (ieq(name, "health")) return static_cast<int>(Builtin::Health);
    if (ieq(name, "wind")) return static_cast<int>(Builtin::Wind);
    if (ieq(name, "mana")) return static_cast<int>(Builtin::Mana);
    if (ieq(name, "neartype")) return static_cast<int>(Builtin::NearType);
    if (ieq(name, "neardist")) return static_cast<int>(Builtin::NearDist);
    if (ieq(name, "nearid")) return static_cast<int>(Builtin::NearId);
    // Round-outcome names from the named-value table @0x82EEF0 (case-insensitive,
    // like every entry — the resolver walks the table with stricmp).
    if (ieq(name, "bluekills")) return static_cast<int>(Builtin::Bluekills);
    if (ieq(name, "greenkills")) return static_cast<int>(Builtin::Greenkills);
    if (ieq(name, "humans")) return static_cast<int>(Builtin::Humans);
    if (ieq(name, "GameOver")) return static_cast<int>(Builtin::GameOver);
    if (ieq(name, "WinVar")) return static_cast<int>(Builtin::WinVar);
    if (ieq(name, "LoseVar")) return static_cast<int>(Builtin::LoseVar);
    if (ieq(name, "accuracyspread")) return static_cast<int>(Builtin::AccuracySpread);
    return -1;
}

Op fold_for(std::string_view op) {
    if (op == "and") return Op::FoldAnd;
    if (op == "or") return Op::FoldOr;
    if (op == "xor") return Op::FoldNe; // boolean xor == (a != b)
    return Op::Call;
}

class Compiler {
public:
    explicit Compiler(const CompileEnv &env) : env_(env) {}

    Program compile(const std::vector<Stmt> &stmts) {
        for (const Stmt &s : stmts) {
            compile_top(s);
        }
        prog_.code.push_back(kProgramTerminator);
        prog_.event_count = event_counter_;
        return std::move(prog_);
    }

private:
    const CompileEnv &env_;
    Program prog_;
    int event_counter_ = 0;

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

    // Resolve an argument to an operand reference word. [orig: WacScript_ResolveParameter.]
    uint32_t resolve(const Arg &arg, ParamType type, int line) {
        const std::string &t = arg.text;

        // String / symbolic-asset params -> string pool, referenced as a pool value.
        bool string_like = arg.is_string || type == ParamType::Text ||
                           type == ParamType::Filename || type == ParamType::SoundSet ||
                           type == ParamType::TextToken;
        if (string_like) {
            int si = intern_string(t);
            return encode_operand(OperandKind::Pool, push_pool(si));
        }

        // Variable lvalue/rvalue: V# mission, G# global, M# music.
        if (!t.empty() && (t[0] == 'V' || t[0] == 'v') && all_digits(std::string_view(t).substr(1))) {
            int idx = std::atoi(t.c_str() + 1);
            if (idx >= world::ScriptVarStore::kMissionVars) { warn(line, "V# too big"); idx = world::ScriptVarStore::kMissionVars - 1; }
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
            // Retail's named-value table stores direct pointers. Port the writable
            // accuracyspread row without pretending the cache-only rows are lvalues.
            // [orig: WacScript_ResolveParameter @0x4f2940; named table @0x82EEF0]
            if (named_value == static_cast<int>(Builtin::AccuracySpread)) {
                return encode_operand(OperandKind::Builtin,
                                      static_cast<uint32_t>(named_value));
            }
            warn(line, "expected a variable");
            return encode_operand(OperandKind::MissionVar, 0);
        }

        // Named engine values (health/ticks/near*/accuracyspread/...).
        if (named_value >= 0) {
            return encode_operand(OperandKind::Builtin,
                                  static_cast<uint32_t>(named_value));
        }

        // Symbolic asset prefixes -> string pool.
        if (starts_with_ci(t, "FX_") || starts_with_ci(t, "effect_") ||
            starts_with_ci(t, "FACE_") || starts_with_ci(t, "ANIM_") ||
            starts_with_ci(t, "anim_") || starts_with_ci(t, "AMMO_") ||
            starts_with_ci(t, "SS_") || starts_with_ci(t, "TT_")) {
            int si = intern_string(t);
            return encode_operand(OperandKind::Pool, push_pool(si));
        }

        // Entity SSN (SSN_<n> or bare net id). We pass the net id; runtime resolves
        // it via EntityRegistry::find_by_net_id (works for late-spawned entities).
        if (starts_with_ci(t, "SSN_")) {
            int net = std::atoi(t.c_str() + 4);
            return encode_operand(OperandKind::Pool, push_pool(net));
        }
        // Named group G_<name>.
        if (starts_with_ci(t, "G_")) {
            int gid = 0;
            std::string_view rest = std::string_view(t).substr(2);
            if (all_digits(rest)) gid = std::atoi(t.c_str() + 2);
            else if (env_.registry) gid = env_.registry->intern_group(rest);
            return encode_operand(OperandKind::Pool, push_pool(gid));
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

        // Numeric literal. Stored as a plain integer (authoring units). Unit-aware
        // 16.16 scaling per WacScript_ResolveParameter (distance/meters/seconds/
        // heading) is a tracked refinement applied where the renderer/physics
        // consume the value; the language machine (var math, temporal, env)
        // operates on the authored integers.
        if (!t.empty() && (std::isdigit(static_cast<unsigned char>(t[0])) || t[0] == '.' || t[0] == '-')) {
            double d = std::atof(t.c_str());
            if (arg.negate) d = -d;
            (void)type;
            return encode_operand(OperandKind::Pool, push_pool(static_cast<int32_t>(d)));
        }

        // Unknown symbol -> intern as a string, best effort.
        int si = intern_string(t);
        return encode_operand(OperandKind::Pool, push_pool(si));
    }

    // Emit a single command call (condition leaf or action). `fold` controls how
    // the return value combines into the accumulator (Assign for the first term).
    void emit_call(Call call, Op fold, bool negate) {
        if (call.name == "$operand") {
            // bare operand used as a condition -> truthiness via `true`.
            call.name = "true";
        }
        int idx = wac_command_index(call.name);
        if (idx < 0) {
            warn(call.line, "unknown command '" + call.name + "'");
            return;
        }
        const CommandDef &def = wac_commands()[idx];
        emit(encode_call(static_cast<uint16_t>(idx), fold, /*push=*/false, negate));
        for (int i = 0; i < def.argc; ++i) {
            ParamType pt = def.params[i];
            if (i < static_cast<int>(call.args.size())) {
                emit(resolve(call.args[i], pt, call.line));
            } else {
                emit(encode_operand(OperandKind::Pool, push_pool(0)));
            }
        }
    }

    void compile_cond(const Expr &e, Op fold) {
        switch (e.kind) {
            case Expr::Leaf:
                emit_call(e.call, fold, /*negate=*/false);
                break;
            case Expr::Not:
                if (!e.kids.empty() && e.kids[0].kind == Expr::Leaf) {
                    emit_call(e.kids[0].call, fold, /*negate=*/true);
                } else if (!e.kids.empty()) {
                    compile_cond(e.kids[0], fold); // best-effort (NOT of a group)
                }
                break;
            case Expr::And:
                if (e.kids.size() >= 2) {
                    compile_cond(e.kids[0], fold);
                    compile_cond(e.kids[1], Op::FoldAnd);
                }
                break;
            case Expr::Or:
                if (e.kids.size() >= 2) {
                    compile_cond(e.kids[0], fold);
                    compile_cond(e.kids[1], Op::FoldOr);
                }
                break;
            case Expr::Xor:
                if (e.kids.size() >= 2) {
                    compile_cond(e.kids[0], fold);
                    compile_cond(e.kids[1], Op::FoldNe);
                }
                break;
        }
    }

    void compile_top(const Stmt &s) {
        if (s.kind == Stmt::If) {
            compile_if(s);
        } else if (s.kind == Stmt::Action) {
            int e = event_counter_++;
            emit(encode_instr(Op::EnterEvent, static_cast<uint32_t>(e)));
            emit_call(s.call, Op::Call, false);
        } else if (s.kind == Stmt::Block) {
            for (const Stmt &b : s.body) compile_top(b);
        }
    }

    // Compile an IF as its own event. Nested IFs inside the body get their own
    // event index; current_event is restored afterward.
    void compile_if(const Stmt &s) {
        int e = event_counter_++;
        emit(encode_instr(Op::EnterEvent, static_cast<uint32_t>(e)));
        compile_cond(s.cond, Op::Call); // accumulator = condition
        size_t andchain = emit(encode_instr(Op::AndChain, 0));
        compile_body(s.body, e);
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

    void compile_body(const std::vector<Stmt> &body, int parent_event) {
        for (const Stmt &st : body) {
            if (st.kind == Stmt::Action) {
                emit_call(st.call, Op::Call, false);
            } else if (st.kind == Stmt::If) {
                compile_if(st);
                // restore current_event for subsequent statements in this body
                emit(encode_instr(Op::EnterEvent, static_cast<uint32_t>(parent_event)));
            } else if (st.kind == Stmt::Block) {
                compile_body(st.body, parent_event); // inline loop body (no iteration yet)
            }
        }
    }

    void compile_else_chain(const Stmt &s, int parent_event) {
        if (!s.elifs.empty()) {
            // Desugar: first elseif becomes an IF whose else is the remaining chain.
            Stmt syn;
            syn.kind = Stmt::If;
            syn.cond = s.elifs[0].cond;
            syn.body = s.elifs[0].body;
            syn.elifs.assign(s.elifs.begin() + 1, s.elifs.end());
            syn.else_body = s.else_body;
            syn.has_else = s.has_else;
            compile_if(syn);
            emit(encode_instr(Op::EnterEvent, static_cast<uint32_t>(parent_event)));
        } else if (s.has_else) {
            compile_body(s.else_body, parent_event);
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
    for (const std::string &src : sources) {
        ParseResult pr = parse(src);
        for (Stmt &s : pr.statements) all.push_back(std::move(s));
        for (Diagnostic &d : pr.diagnostics) parse_diags.push_back(std::move(d));
    }
    Program prog = compile(all, env);
    prog.diagnostics.insert(prog.diagnostics.begin(), parse_diags.begin(), parse_diags.end());
    return prog;
}

} // namespace opennova::wac
