#include <formats/wac/parser.h>

#include <array>

#include <formats/wac/command.h>
#include <formats/wac/lexer.h>

// [orig: Script_Compile @0x4f31f0 — the shunting-yard statement/expression parser (operator
//  precedence via Script_GetOperatorPrecedence, parameters via WacScript_ResolveParameter @0x4f2920)]

namespace opennova::wac {
namespace {

// Marker name for a bare operand used directly as a condition / comparison side.
constexpr const char *kOperandMarker = "$operand";

const char *cmp_command(std::string_view op) {
    if (op == "==") return "eq";
    if (op == "!=" || op == "<>" || op == "~=") return "ne";
    if (op == "<") return "lt";
    if (op == ">") return "gt";
    if (op == "<=") return "le";
    if (op == ">=") return "ge";
    return nullptr;
}

class Parser {
public:
    explicit Parser(std::string_view src) : toks_(lex(src)) {}

    ParseResult run() {
        ParseResult r;
        while (!at_end()) {
            Stmt s;
            if (parse_statement(s)) {
                r.statements.push_back(std::move(s));
            }
        }
        r.diagnostics = std::move(diags_);
        return r;
    }

private:
    std::vector<Token> toks_;
    size_t pos_ = 0;
    std::vector<Diagnostic> diags_;

    const Token &cur() const { return toks_[pos_]; }
    bool at_end() const { return cur().kind == TokKind::End; }
    void advance() { if (!at_end()) ++pos_; }

    bool is_kw(std::string_view low) const {
        return cur().kind == TokKind::Keyword && cur().lowered == low;
    }
    bool next_is_kw(std::string_view low) const {
        if (pos_ + 1 >= toks_.size()) return false;
        const Token &t = toks_[pos_ + 1];
        return t.kind == TokKind::Keyword && t.lowered == low;
    }
    bool is_op(std::string_view low) const {
        return cur().kind == TokKind::Operator && cur().lowered == low;
    }
    void warn(const std::string &msg) {
        diags_.push_back(Diagnostic{cur().line, cur().col, msg, false});
    }
    void error(const std::string &msg) {
        diags_.push_back(Diagnostic{cur().line, cur().col, msg, true});
    }

    bool kw_in(std::initializer_list<const char *> set) const {
        if (cur().kind != TokKind::Keyword) return false;
        for (const char *k : set) {
            if (cur().lowered == k) return true;
        }
        return false;
    }

    // ---- statements ----
    bool parse_statement(Stmt &out) {
        if (at_end()) return false;
        if (is_kw("if")) return parse_if(out);
        if (kw_in({"do", "dornd", "doseq", "ploop", "gloop"})) return parse_block(out);
        if (kw_in({"var", "array", "run"})) {
            out.kind = is_kw("run") ? Stmt::Run : Stmt::Declaration;
            out.array_declaration = is_kw("array");
            out.call.line = cur().line;
            advance();
            if (cur().kind != TokKind::Word && cur().kind != TokKind::String) {
                error("expected a name");
                return false;
            }
            out.call.name = cur().text;
            advance();
            return true;
        }
        if (kw_in({"else", "elseif", "endif", "end", "enif", "then", "next", "enddo"})) {
            // Stray block terminator at statement scope; skip.
            advance();
            return false;
        }
        if (cur().kind == TokKind::Word) {
            if (pos_ + 1 < toks_.size() && toks_[pos_ + 1].text == "=") {
                out.kind = Stmt::Assignment;
                out.assignment_target.text = cur().text;
                out.call.line = cur().line;
                advance();
                advance();
                out.cond = parse_expr(&out.assignment_target);
            } else {
                out.cond = parse_expr();
                if (out.cond.kind == Expr::Leaf && out.cond.call.name != kOperandMarker &&
                        out.cond.op.empty() && !out.cond.negate && !out.cond.push) {
                    out.kind = Stmt::Action;
                    out.call = out.cond.call;
                } else out.kind = Stmt::Expression;
            }
            return true;
        }
        if (cur().kind == TokKind::LParen || is_op("-") || is_op("not") || is_op("!")) {
            out.kind = Stmt::Expression;
            out.cond = parse_expr();
            return true;
        }
        // Anything else (stray operator/paren) — skip to recover.
        advance();
        return false;
    }

    IfMode parse_if_mode() {
        IfMode mode = IfMode::Then;
        if (is_kw("enter")) mode = IfMode::Enter;
        else if (is_kw("leave")) mode = IfMode::Leave;
        else if (!is_kw("then")) {
            warn("expected 'then', 'enter' or 'leave'");
            return mode;
        }
        advance();
        return mode;
    }

    void parse_event_name(std::string &name) {
        if (cur().kind != TokKind::LBracket) return;
        advance();
        if (cur().kind == TokKind::Word || cur().kind == TokKind::String) {
            name = cur().text;
            advance();
        } else error("expected an event name");
        if (cur().kind == TokKind::RBracket) advance();
        else error("expected ']'");
    }

    bool parse_if(Stmt &out) {
        out.kind = Stmt::If;
        advance(); // 'if'
        parse_event_name(out.event_name);
        out.cond = parse_expr();
        parse_event_name(out.event_name);
        out.mode = parse_if_mode();
        out.body = parse_block_until({"else", "elseif", "endif", "end", "enif"});
        // Retail chains alternatives as `elseif` OR the two-word `else if`
        // (the shipped corpus' dominant form — e.g. 00TRg.wac), both closed by
        // the chain's ONE endif. `else` directly followed by `if` is that
        // chain, never a nested if: nesting would hand the single endif to the
        // inner if and fold every trailing statement into the else branch.
        while (is_kw("elseif") || (is_kw("else") && next_is_kw("if"))) {
            if (is_kw("elseif")) {
                advance();
            } else {
                advance(); // 'else'
                advance(); // 'if'
            }
            ElseIf ei;
            parse_event_name(ei.event_name);
            ei.cond = parse_expr();
            parse_event_name(ei.event_name);
            ei.mode = parse_if_mode();
            ei.body = parse_block_until({"else", "elseif", "endif", "end", "enif"});
            out.elifs.push_back(std::move(ei));
        }
        if (is_kw("else")) {
            advance();
            out.has_else = true;
            out.else_body = parse_block_until({"endif", "end", "enif"});
        }
        if (kw_in({"endif", "end", "enif"})) {
            advance();
        } else {
            warn("expected 'endif'");
        }
        return true;
    }

    bool parse_block(Stmt &out) {
        out.kind = Stmt::Block;
        out.block_kind = cur().lowered;
        out.call.line = cur().line;
        advance();
        if (out.block_kind == "gloop") {
            // The GLOOP operand is the NEXT token, whatever it is: retail's
            // tokenizer splits on its 20-byte operator set (`{}()[]+-*/|&^%<>=!~`
            // @0x7CE2E8), so `gloop(G_x)` lexes as GLOOP `(` G_X `)` and the
            // `(` becomes the group token (an Unknown Group -> group 0 in the
            // compiler); the leftover `G_x` `)` are statements of the body.
            // [orig: Script_Compile tokenizer @0x4f32e0..0x4f3464 (operator
            //  tests @0x4f3370/@0x4f3448); the GLOOP operand resolve
            //  @0x4f365d..0x4f3693]
            if (!parse_arg(out.block_argument)) {
                if (at_end()) {
                    error("expected a group after GLOOP");
                } else {
                    out.block_argument.text = cur().text;
                    advance();
                }
            }
        }
        out.body = parse_block_until({"next", "enddo", "endif", "end", "enif"});
        while (is_kw("next")) {
            advance();
            out.next_bodies.push_back(parse_block_until({"next", "enddo", "endif", "end", "enif"}));
        }
        if (kw_in({"enddo", "endif", "end", "enif"})) advance();
        return true;
    }

    std::vector<Stmt> parse_block_until(std::initializer_list<const char *> stop) {
        std::vector<Stmt> body;
        while (!at_end() && !kw_in(stop)) {
            // 'next' inside a loop body separates sections; consume + continue.
            if (is_kw("next")) { advance(); continue; }
            Stmt s;
            size_t before = pos_;
            if (parse_statement(s)) body.push_back(std::move(s));
            if (pos_ == before) advance(); // guard against no-progress
        }
        return body;
    }

    // ---- calls ----
    Call parse_call() {
        Call c;
        c.name = cur().text;
        c.line = cur().line;
        advance();
        const CommandDef *def = wac_find_command(c.name);
        if (cur().kind == TokKind::LParen) {
            advance();
            while (!at_end() && cur().kind != TokKind::RParen) {
                Arg a;
                if (parse_arg(a)) c.args.push_back(std::move(a));
                if (cur().kind == TokKind::Comma) advance();
                else if (cur().kind != TokKind::RParen) {
                    // tolerate stray tokens inside the arg list
                    if (cur().kind != TokKind::Word && cur().kind != TokKind::String &&
                        !is_op("-")) {
                        break;
                    }
                }
            }
            if (cur().kind == TokKind::RParen) advance();
        } else {
            // Paren-less: collect up to argc primaries (e.g. `chain 7`, `random 3`).
            int want = def ? def->argc : 0;
            for (int i = 0; i < want; ++i) {
                if (!arg_starts_here()) break;
                Arg a;
                if (!parse_arg(a)) break;
                c.args.push_back(std::move(a));
                if (cur().kind == TokKind::Comma) advance();
            }
        }
        return c;
    }

    bool arg_starts_here() const {
        return cur().kind == TokKind::Word || cur().kind == TokKind::String || is_op("-");
    }

    bool parse_arg(Arg &out) {
        if (is_op("-")) {
            // [orig: Script_Compile @0x4F31F0] With a pending operator or
            // parameter, '-' begins a word. Whitespace ends that word:
            // -V1 resolves numerically to zero; '- V1' is two parameters.
            const Token minus = cur();
            out.text = "-";
            advance();
            if (cur().kind == TokKind::Word && cur().line == minus.line &&
                    cur().col == minus.col + 1) {
                out.text += cur().text;
                advance();
            }
            return true;
        }
        if (cur().kind == TokKind::String) {
            out.text = cur().text;
            out.is_string = true;
            advance();
            return true;
        }
        if (cur().kind == TokKind::Word) {
            out.text = cur().text;
            advance();
            return true;
        }
        return false;
    }

    // [orig: Script_GetOperatorPrecedence @0x4EE540] AND and OR share
    // precedence 1; comparisons 2, +/- 3, */% 4, power 5.
    static int precedence(std::string_view op) {
        if (op == "and" || op == "or" || op == "&&" || op == "||" || op == "xor") return 1;
        if (cmp_command(op)) return 2;
        if (op == "+" || op == "-") return 3;
        if (op == "*" || op == "/" || op == "%") return 4;
        if (op == "^") return 5;
        return 0;
    }

    // [orig: Script_Compile @0x4F31F0]
    // The lookahead is tokenized with the CURRENT pending operator. Calls
    // clear that operator before their parameters are read, and parameters
    // suppress auto parentheses. A falling precedence does not pop a frame.
    Expr parse_expr(const Arg *assignment = nullptr) {
        struct Frame { std::string op; bool negate; int level; bool automatic; };
        std::vector<Frame> frames;
        Expr result;
        result.kind = Expr::Sequence;
        std::string pending;
        bool negate = false;
        bool push = false;
        int level = 0;
        bool assigned = false;
        auto store_assignment = [&] {
            if (!assignment || !assigned) return;
            Expr edge;
            edge.kind = Expr::Store;
            edge.call.line = cur().line;
            edge.call.args.push_back(*assignment);
            result.kids.push_back(std::move(edge));
            assignment = nullptr;
        };
        auto reset = [&] { pending.clear(); negate = false; push = false; level = 0; };
        auto save = [&](bool automatic) {
            if (frames.size() >= 16) warn(automatic ? "Auto Paren nesting too deep" : "Paren nesting too deep");
            frames.push_back({pending, negate, level, automatic});
            if (!pending.empty() || negate) { reset(); push = true; }
        };
        auto pop = [&](const Frame &frame) {
            Expr edge;
            edge.kind = Expr::Pop;
            edge.op = frame.op;
            edge.negate = frame.negate;
            result.kids.push_back(std::move(edge));
            reset();
        };
        // The lookahead outlives the loop: retail computes it per token
        // (the boundary keyword included) and the boundary/EOF drain below
        // compares every frame against the last value.
        int lookahead = 0;
        while (!at_end()) {
            const bool signed_word = is_op("-") && !pending.empty();
            size_t after = pos_ + 1;
            if (signed_word && after < toks_.size() && toks_[after].kind == TokKind::Word &&
                    toks_[after].line == cur().line && toks_[after].col == cur().col + 1) ++after;
            const Token &next = toks_[after < toks_.size() ? after : toks_.size() - 1];
            const bool next_symbol = next.kind == TokKind::Operator && !is_word_operator(next.lowered);
            lookahead = next_symbol && !(next.text == "-" && !pending.empty())
                    ? precedence(next.lowered) : 0;
            const bool boundary = cur().kind == TokKind::Keyword || cur().kind == TokKind::LBracket ||
                    (cur().kind == TokKind::Word && pos_ + 1 < toks_.size() && toks_[pos_ + 1].text == "=");
            if (boundary) {
                // Retail commits a pending variable before the boundary's
                // auto-paren drain. EOF instead drains before the final store.
                if (pending.empty()) store_assignment();
                break;
            }
            if (!push && !pending.empty() && lookahead > level) save(true);

            if (cur().kind == TokKind::LParen) {
                save(false);
                advance();
                continue;
            }
            if (cur().kind == TokKind::RParen) {
                advance();
                if (frames.empty()) { warn("Unexpected )"); continue; }
                Frame frame = std::move(frames.back());
                frames.pop_back();
                if (!frame.op.empty() || frame.negate) {
                    if (lookahead <= frame.level) pop(frame);
                    else { frame.automatic = true; frames.push_back(std::move(frame)); }
                }
                continue;
            }
            if (!signed_word && cur().kind == TokKind::Operator) {
                if (is_op("not") || is_op("!")) {
                    if (negate) warn("Unexpected NOT");
                    else negate = true;
                } else if (precedence(cur().lowered)) {
                    if (!pending.empty() || negate) warn("Unexpected " + cur().text);
                    else { pending = cur().lowered; level = precedence(pending); }
                } else warn("Unexpected " + cur().text);
                advance();
                continue;
            }
            // [orig: Script_Compile @0x4F4019..0x4F404B] This runs AFTER
            // auto/explicit grouping cleared the operator, even before the
            // first grouped call. The assignment can therefore store early.
            if (pending.empty()) store_assignment();
            Expr leaf;
            leaf.op = pending;
            leaf.negate = negate;
            leaf.push = push;
            leaf.call.line = cur().line;
            if (cur().kind == TokKind::Word &&
                    (wac_find_command(cur().text) || next.kind == TokKind::LParen)) {
                leaf.call = parse_call();
            } else {
                leaf.call.name = kOperandMarker;
                Arg arg;
                if (!parse_arg(arg)) { advance(); continue; }
                leaf.call.args.push_back(std::move(arg));
            }
            result.kids.push_back(std::move(leaf));
            assigned = true;
            reset();
        }
        while (!frames.empty()) {
            const Frame frame = std::move(frames.back());
            frames.pop_back();
            if (!frame.automatic) warn("Open Paren");
            // A frame whose stored precedence is BELOW the lookahead is
            // dropped WITHOUT its POP and the rest of the drain is abandoned:
            // retail jumps straight back to the tokenizer head. Malformed
            // input only (an operator right after the keyword / at EOF).
            // [orig: Script_Compile — the keyword drains @0x4f4226..0x4f4231
            //  / @0x4f4818..0x4f4823 and the EOF drain @0x4f559f..0x4f55aa:
            //  `cmp cl,[esp+esi+precStack]; ja loc_4F32E0` ahead of the
            //  `add eax,7000000h` POP emit @0x4f55b7..0x4f55c4]
            if (lookahead > frame.level) break;
            pop(frame);
        }
        store_assignment();
        if (result.kids.size() == 1) return std::move(result.kids.front());
        return result;
    }

};

} // namespace

ParseResult parse(std::string_view source) {
    Parser p(source);
    return p.run();
}

} // namespace opennova::wac
